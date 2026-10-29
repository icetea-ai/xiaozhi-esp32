// main/boards/tuni-p4/tuni_p4_audio_codec.cc
#include "tuni_p4_audio_codec.h"

#include <esp_log.h>
#include <cmath>
#include <vector>

#define TAG "TuniP4AudioCodec"

TuniP4AudioCodec::TuniP4AudioCodec(int input_sample_rate, int output_sample_rate,
    gpio_num_t bclk, gpio_num_t ws, gpio_num_t spk_dout, gpio_num_t mic_din) {
    // Mics and amps share one clock, so both directions run at one rate.
    if (input_sample_rate != output_sample_rate) {
        ESP_LOGE(TAG, "Shared I2S clock: input %d Hz != output %d Hz", input_sample_rate, output_sample_rate);
    }
    duplex_ = true;
    input_reference_ = false;
    input_channels_ = 1;  // L+R mics are averaged in Read()
    input_sample_rate_ = input_sample_rate;
    output_sample_rate_ = output_sample_rate;

    i2s_chan_config_t chan_cfg = {
        .id = I2S_NUM_1,
        .role = I2S_ROLE_MASTER,
        .dma_desc_num = AUDIO_CODEC_DMA_DESC_NUM,
        .dma_frame_num = AUDIO_CODEC_DMA_FRAME_NUM,
        .auto_clear_after_cb = true,
        .auto_clear_before_cb = false,
        .intr_priority = 0,
    };
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &tx_handle_, &rx_handle_));

    // 32-bit slots: the ICS-43434 needs 64 BCLKs per frame and sends 24-bit
    // data MSB-aligned in each slot. The MAX98357A detects 64 fs on its own.
    i2s_std_config_t std_cfg = {
        .clk_cfg = {
            .sample_rate_hz = (uint32_t)output_sample_rate_,
            .clk_src = I2S_CLK_SRC_DEFAULT,
            .mclk_multiple = I2S_MCLK_MULTIPLE_256,
            #ifdef I2S_HW_VERSION_2
                .ext_clk_freq_hz = 0,
            #endif
        },
        .slot_cfg = {
            .data_bit_width = I2S_DATA_BIT_WIDTH_32BIT,
            .slot_bit_width = I2S_SLOT_BIT_WIDTH_AUTO,
            .slot_mode = I2S_SLOT_MODE_MONO,
            .slot_mask = I2S_STD_SLOT_BOTH,
            .ws_width = I2S_DATA_BIT_WIDTH_32BIT,
            .ws_pol = false,
            .bit_shift = true,
            #ifdef I2S_HW_VERSION_2
                .left_align = true,
                .big_endian = false,
                .bit_order_lsb = false
            #endif
        },
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = bclk,
            .ws = ws,
            .dout = spk_dout,
            .din = mic_din,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false
            }
        }
    };
    // TX: mono + both slots, so each sample goes out on L and R (per the IDF
    // STD TX table for non-ESP32 chips) and both amps play it.
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(tx_handle_, &std_cfg));
    // RX: stereo, left = mic 1, right = mic 2.
    std_cfg.slot_cfg.slot_mode = I2S_SLOT_MODE_STEREO;
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(rx_handle_, &std_cfg));
    ESP_LOGI(TAG, "I2S1 duplex: BCLK=%d WS=%d DOUT=%d (MAX98357A) DIN=%d (ICS-43434), %d Hz",
             bclk, ws, spk_dout, mic_din, output_sample_rate_);
}

TuniP4AudioCodec::~TuniP4AudioCodec() {
    if (input_enabled_) {
        i2s_channel_disable(rx_handle_);
    }
    if (input_enabled_ || output_enabled_) {
        i2s_channel_disable(tx_handle_);
    }
    i2s_del_channel(rx_handle_);
    i2s_del_channel(tx_handle_);
}

// The mics are clocked by the shared BCLK/WS, so TX runs whenever either
// direction is on. With output off it sends zeros (auto_clear_after_cb);
// only with both off does BCLK stop and the MAX98357As enter clock-loss
// shutdown.
void TuniP4AudioCodec::EnableInput(bool enable) {
    std::lock_guard<std::mutex> lock(data_if_mutex_);
    if (enable == input_enabled_) {
        return;
    }
    if (enable) {
        if (!output_enabled_) {
            ESP_ERROR_CHECK(i2s_channel_enable(tx_handle_));
        }
        ESP_ERROR_CHECK(i2s_channel_enable(rx_handle_));
    } else {
        ESP_ERROR_CHECK(i2s_channel_disable(rx_handle_));
        if (!output_enabled_) {
            ESP_ERROR_CHECK(i2s_channel_disable(tx_handle_));
        }
    }
    AudioCodec::EnableInput(enable);
}

void TuniP4AudioCodec::EnableOutput(bool enable) {
    std::lock_guard<std::mutex> lock(data_if_mutex_);
    if (enable == output_enabled_) {
        return;
    }
    if (!input_enabled_) {
        if (enable) {
            ESP_ERROR_CHECK(i2s_channel_enable(tx_handle_));
        } else {
            ESP_ERROR_CHECK(i2s_channel_disable(tx_handle_));
        }
    }
    AudioCodec::EnableOutput(enable);
}

int TuniP4AudioCodec::Read(int16_t* dest, int samples) {
    constexpr uint32_t kReadTimeoutMs = 200;
    std::vector<int32_t> buffer(samples * 2);
    size_t bytes_read;
    if (i2s_channel_read(rx_handle_, buffer.data(), buffer.size() * sizeof(int32_t), &bytes_read, kReadTimeoutMs) != ESP_OK) {
        return 0;
    }
    samples = bytes_read / sizeof(int32_t) / 2;

    // input_gain_ is digital gain in dB on top of the mics' -26 dBFS @ 94 dB SPL.
    // Work in Q8 of the int16 range: (x >> 8) keeps 24 bits of mic data.
    int32_t gain_q8 = std::lround(std::pow(10.0, input_gain_ / 20.0) * 256);
    for (int i = 0; i < samples; i++) {
        int64_t mono = ((int64_t)(buffer[2 * i] >> 8) + (buffer[2 * i + 1] >> 8)) / 2;
        int64_t value = (mono * gain_q8) >> 16;
        dest[i] = (value > INT16_MAX) ? INT16_MAX : (value < -INT16_MAX) ? -INT16_MAX : (int16_t)value;
    }
    return samples;
}

int TuniP4AudioCodec::Write(const int16_t* data, int samples) {
    std::lock_guard<std::mutex> lock(data_if_mutex_);
    if (!output_enabled_) {
        return samples;
    }
    std::vector<int32_t> buffer(samples);

    // The amps have no volume control: scale in software. Q16 factor, so the
    // product is the sample already MSB-aligned in the 32-bit slot.
    int32_t volume_factor = pow(double(output_volume_) / 100.0, 2) * 65536;
    for (int i = 0; i < samples; i++) {
        buffer[i] = int32_t(data[i]) * volume_factor;
    }

    size_t bytes_written;
    ESP_ERROR_CHECK(i2s_channel_write(tx_handle_, buffer.data(), samples * sizeof(int32_t), &bytes_written, portMAX_DELAY));
    return bytes_written / sizeof(int32_t);
}
