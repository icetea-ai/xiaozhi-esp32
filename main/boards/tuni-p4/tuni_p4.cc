// main/boards/tuni-p4/tuni_p4.cc
#include "wifi_board.h"
#include "codecs/es8311_audio_codec.h"
#include "application.h"
#include "display/lcd_display.h"
#include "button.h"
#include "config.h"
#include "ssid_manager.h"

#include <esp_log.h>
#include <driver/i2c_master.h>
#include <esp_ldo_regulator.h>
#include <esp_lcd_mipi_dsi.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_panel_interface.h>
#include <esp_lcd_jd9365.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include "lcd_init_cmds.h"

#define TAG "TuniP4"

// How long the JD9365 init may take before the panel is treated as absent.
// A connected panel finishes in well under a second (the longest delay in the
// init sequence is the 120 ms sleep-out).
#define LCD_INIT_TIMEOUT_MS 2000

// The DPI panel has no set_gap(), so the 480x480 window is placed by wrapping
// the panel's draw_bitmap and shifting every flush area into the window.
static esp_err_t (*s_panel_draw_bitmap)(esp_lcd_panel_t*, int, int, int, int, const void*) = nullptr;

static esp_err_t DrawBitmapInWindow(esp_lcd_panel_t* panel, int x_start, int y_start,
                                    int x_end, int y_end, const void* color_data) {
    return s_panel_draw_bitmap(panel, x_start + DISPLAY_OFFSET_X, y_start + DISPLAY_OFFSET_Y,
                               x_end + DISPLAY_OFFSET_X, y_end + DISPLAY_OFFSET_Y, color_data);
}

// esp_lcd_panel_init() spins forever in the DSI read of the panel ID when no
// panel is attached, so it runs in a task on the other core that the board can
// abandon. Static: the task may still be running when the constructor returns.
static struct {
    esp_lcd_panel_handle_t panel;
    SemaphoreHandle_t done;
    esp_err_t err;
} s_lcd_init;

static void LcdInitTask(void*) {
    s_lcd_init.err = esp_lcd_panel_reset(s_lcd_init.panel);
    if (s_lcd_init.err == ESP_OK) {
        s_lcd_init.err = esp_lcd_panel_init(s_lcd_init.panel);
    }
    xSemaphoreGive(s_lcd_init.done);
    vTaskSuspend(nullptr);  // deleted by the waiting side
}

class TuniP4 : public WifiBoard {
private:
    i2c_master_bus_handle_t codec_i2c_bus_;
    Button boot_button_;
    Display* display_ = nullptr;

    void InitializeCodecI2c() {
        i2c_master_bus_config_t i2c_bus_cfg = {
            .i2c_port = I2C_NUM_1,
            .sda_io_num = AUDIO_CODEC_I2C_SDA_PIN,
            .scl_io_num = AUDIO_CODEC_I2C_SCL_PIN,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .intr_priority = 0,
            .trans_queue_depth = 0,
            .flags = { .enable_internal_pullup = 1 },
        };
        ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_cfg, &codec_i2c_bus_));
    }

    // Best effort: the Waveshare panel's power/backlight MCU at 0x45. It does
    // not ACK on every unit and the panel lights up without it.
    void PowerOnPanel() {
        const uint8_t cmds[][2] = {{0x95, 0x11}, {0x95, 0x17}, {0x96, 0x00}, {0x96, 0xFF}};
        i2c_device_config_t dev_cfg = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = 0x45,
            .scl_speed_hz = 100000,
        };
        i2c_master_dev_handle_t dev = nullptr;
        if (i2c_master_bus_add_device(codec_i2c_bus_, &dev_cfg, &dev) != ESP_OK) {
            return;
        }
        for (auto& cmd : cmds) {
            i2c_master_transmit(dev, cmd, sizeof(cmd), 50);
        }
        i2c_master_bus_rm_device(dev);
    }

    bool InitializeLcd() {
        PowerOnPanel();

        esp_ldo_channel_handle_t phy_pwr_chan = nullptr;
        esp_ldo_channel_config_t ldo_cfg = {
            .chan_id = MIPI_DSI_PHY_PWR_LDO_CHAN,
            .voltage_mv = MIPI_DSI_PHY_PWR_LDO_VOLTAGE_MV,
        };
        ESP_ERROR_CHECK(esp_ldo_acquire_channel(&ldo_cfg, &phy_pwr_chan));

        esp_lcd_dsi_bus_handle_t dsi_bus = nullptr;
        esp_lcd_dsi_bus_config_t bus_cfg = {
            .bus_id = 0,
            .num_data_lanes = LCD_MIPI_DSI_LANE_NUM,
            .lane_bit_rate_mbps = 1500,
        };
        ESP_ERROR_CHECK(esp_lcd_new_dsi_bus(&bus_cfg, &dsi_bus));

        esp_lcd_panel_io_handle_t io = nullptr;
        esp_lcd_dbi_io_config_t dbi_cfg = JD9365_PANEL_IO_DBI_CONFIG();
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_dbi(dsi_bus, &dbi_cfg, &io));

        esp_lcd_dpi_panel_config_t dpi_cfg = {
            .virtual_channel = 0,
            .dpi_clk_src = MIPI_DSI_DPI_CLK_SRC_DEFAULT,
            .dpi_clock_freq_mhz = 80,
            .in_color_format = LCD_COLOR_FMT_RGB565,
            .num_fbs = 1,
            .video_timing = {
                .h_size = LCD_PANEL_H_RES,
                .v_size = LCD_PANEL_V_RES,
                .hsync_pulse_width = 20,
                .hsync_back_porch = 20,
                .hsync_front_porch = 40,
                .vsync_pulse_width = 10,
                .vsync_back_porch = 4,
                .vsync_front_porch = 30,
            },
        };
        jd9365_vendor_config_t vendor_cfg = {
            .init_cmds = lcd_init_cmds,
            .init_cmds_size = sizeof(lcd_init_cmds) / sizeof(lcd_init_cmds[0]),
            .mipi_config = {
                .dsi_bus = dsi_bus,
                .dpi_config = &dpi_cfg,
                .lane_num = LCD_MIPI_DSI_LANE_NUM,
            },
        };
        esp_lcd_panel_dev_config_t dev_cfg = {
            .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
            .bits_per_pixel = 16,
            .reset_gpio_num = LCD_PIN_NUM_RST,
            .vendor_config = &vendor_cfg,
        };
        esp_lcd_panel_handle_t panel = nullptr;
        ESP_ERROR_CHECK(esp_lcd_new_panel_jd9365(io, &dev_cfg, &panel));
        ESP_ERROR_CHECK(esp_lcd_dpi_panel_enable_dma2d(panel));

        s_lcd_init.panel = panel;
        s_lcd_init.done = xSemaphoreCreateBinary();
        TaskHandle_t task = nullptr;
        // Core 1 and low priority: while it spins on a missing panel it must
        // not starve this (core 0) task's timeout.
        xTaskCreatePinnedToCore(LcdInitTask, "lcd_init", 4096, nullptr, 1, &task, 1);
        bool finished = xSemaphoreTake(s_lcd_init.done, pdMS_TO_TICKS(LCD_INIT_TIMEOUT_MS)) == pdTRUE;
        vTaskDelete(task);
        if (!finished) {
            ESP_LOGW(TAG, "JD9365 init timed out, no panel attached? Running without display");
            return false;
        }
        if (s_lcd_init.err != ESP_OK) {
            ESP_LOGE(TAG, "JD9365 init failed: %s", esp_err_to_name(s_lcd_init.err));
            return false;
        }

        s_panel_draw_bitmap = panel->draw_bitmap;
        panel->draw_bitmap = DrawBitmapInWindow;

        display_ = new MipiLcdDisplay(io, panel, DISPLAY_WIDTH, DISPLAY_HEIGHT, 0, 0,
                                      DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY);
        ESP_LOGI(TAG, "LCD %dx%d window at (%d,%d) on %dx%d panel", DISPLAY_WIDTH, DISPLAY_HEIGHT,
                 DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y, LCD_PANEL_H_RES, LCD_PANEL_V_RES);
        return true;
    }

    void InitializeButtons() {
        // No press-to-talk. Short click only enters Wi-Fi config while still starting.
        boot_button_.OnClick([this]() {
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateStarting) {
                EnterWifiConfigMode();
            }
        });

        // Long-press (any state) forgets the stored network(s) and re-enters BLE
        // pairing - a full re-provision, not just a temporary config-mode visit.
        // Scoped to the "wifi" NVS namespace only (SsidManager::Clear()), so the
        // device identity keypair / JWT state survive - this is not a factory
        // reset. EnterWifiConfigMode() already handles the graceful teardown
        // when already connected (closes the audio channel, stops station,
        // then starts BLE advertising).
        boot_button_.OnLongPress([this]() {
            ESP_LOGW(TAG, "BOOT long-press: forgetting WiFi credentials");
            SsidManager::GetInstance().Clear();
            EnterWifiConfigMode();
        });
    }

public:
    TuniP4() : boot_button_(BOOT_BUTTON_GPIO, false, 5000) {
        InitializeCodecI2c();
        InitializeButtons();
        if (!InitializeLcd()) {
            display_ = new NoDisplay();
        }
    }

    virtual AudioCodec* GetAudioCodec() override {
        static Es8311AudioCodec audio_codec(
            codec_i2c_bus_, I2C_NUM_1, AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_GPIO_MCLK, AUDIO_I2S_GPIO_BCLK, AUDIO_I2S_GPIO_WS,
            AUDIO_I2S_GPIO_DOUT, AUDIO_I2S_GPIO_DIN,
            AUDIO_CODEC_PA_PIN, AUDIO_CODEC_ES8311_ADDR);
        // Far-field open mic: bump ES8311 ADC PGA from the 30 dB default to 36 dB
        // (+6 dB ≈ 2x) for louder capture (helps STT + pronunciation scoring).
        // PGA steps are 0/6/.../42 dB; 36 keeps headroom so loud speech doesn't clip.
        // Applied on the first EnableInput (UpdateDeviceState), set here before that.
        audio_codec.SetInputGain(36.0f);
        // Speaker too quiet to hear the TTS clearly: force output volume to max
        // (default is 70/100). Overrides any stored NVS value on boot so it's
        // reliably loud; applied on the first EnableOutput.
        audio_codec.SetOutputVolume(100);
        return &audio_codec;
    }

    virtual Display* GetDisplay() override { return display_; }
};

DECLARE_BOARD(TuniP4);
