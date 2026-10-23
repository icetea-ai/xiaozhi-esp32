// main/boards/tuni-p4/tuni_p4_audio_codec.cc
#include "tuni_p4_audio_codec.h"

#include <esp_log.h>
#include <cmath>
#include <vector>

#define TAG "TuniP4AudioCodec"

TuniP4AudioCodec::TuniP4AudioCodec(Es8311AudioCodec* mic, int output_sample_rate,
    gpio_num_t spk_bclk, gpio_num_t spk_ws, gpio_num_t spk_dout) : mic_(mic) {
    // Mic and speakers sit on separate I2S peripherals, so the speaker clock
    // can stop when idle without stalling capture.
    duplex_ = false;
    input_reference_ = false;
    input_sample_rate_ = mic->input_sample_rate();
    output_sample_rate_ = output_sample_rate;
    input_gain_ = mic->input_gain();

    // I2S0 belongs to the ES8311.
    i2s_chan_config_t chan_cfg = {
        .id = I2S_NUM_1,
        .role = I2S_ROLE_MASTER,
        .dma_desc_num = AUDIO_CODEC_DMA_DESC_NUM,
        .dma_frame_num = AUDIO_CODEC_DMA_FRAME_NUM,
        .auto_clear_after_cb = true,
        .auto_clear_before_cb = false,
        .intr_priority = 0,
    };
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &tx_handle_, nullptr));

    // Mono + both slots: each sample goes out on L and R (per the IDF STD TX
    // table for non-ESP32 chips), so both amps play regardless of strapping.
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
            .data_bit_width = I2S_DATA_BIT_WIDTH_16BIT,
            .slot_bit_width = I2S_SLOT_BIT_WIDTH_AUTO,
            .slot_mode = I2S_SLOT_MODE_MONO,
            .slot_mask = I2S_STD_SLOT_BOTH,
            .ws_width = I2S_DATA_BIT_WIDTH_16BIT,
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
            .bclk = spk_bclk,
            .ws = spk_ws,
            .dout = spk_dout,
            .din = I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false
            }
        }
    };
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(tx_handle_, &std_cfg));
    ESP_LOGI(TAG, "MAX98357A output on I2S1: BCLK=%d WS=%d DOUT=%d, %d Hz",
             spk_bclk, spk_ws, spk_dout, output_sample_rate_);
}

TuniP4AudioCodec::~TuniP4AudioCodec() {
    if (output_enabled_) {
        i2s_channel_disable(tx_handle_);
    }
    i2s_del_channel(tx_handle_);
}

bool TuniP4AudioCodec::InputData(std::vector<int16_t>& data) {
    return mic_->InputData(data);
}

// Unused: InputData() goes straight to the ES8311.
int TuniP4AudioCodec::Read(int16_t* dest, int samples) {
    return 0;
}

void TuniP4AudioCodec::SetInputGain(float gain) {
    mic_->SetInputGain(gain);
    AudioCodec::SetInputGain(gain);
}

void TuniP4AudioCodec::EnableInput(bool enable) {
    mic_->EnableInput(enable);
    AudioCodec::EnableInput(enable);
}

void TuniP4AudioCodec::EnableOutput(bool enable) {
    std::lock_guard<std::mutex> lock(tx_mutex_);
    if (enable == output_enabled_) {
        return;
    }
    // Stopping BCLK puts the MAX98357A into its clock-loss shutdown, so the
    // speakers don't hiss between replies.
    if (enable) {
        ESP_ERROR_CHECK(i2s_channel_enable(tx_handle_));
    } else {
        ESP_ERROR_CHECK(i2s_channel_disable(tx_handle_));
    }
    AudioCodec::EnableOutput(enable);
}

int TuniP4AudioCodec::Write(const int16_t* data, int samples) {
    std::lock_guard<std::mutex> lock(tx_mutex_);
    if (!output_enabled_) {
        return samples;
    }
    std::vector<int16_t> buffer(samples);

    // The amps have no volume control: scale in software.
    int32_t volume_factor = pow(double(output_volume_) / 100.0, 2) * 65536;
    for (int i = 0; i < samples; i++) {
        buffer[i] = static_cast<int16_t>((int32_t(data[i]) * volume_factor) >> 16);
    }

    size_t bytes_written;
    ESP_ERROR_CHECK(i2s_channel_write(tx_handle_, buffer.data(), samples * sizeof(int16_t), &bytes_written, portMAX_DELAY));
    return bytes_written / sizeof(int16_t);
}
