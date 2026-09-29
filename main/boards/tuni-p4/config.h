// main/boards/tuni-p4/config.h
#ifndef _BOARD_CONFIG_H_
#define _BOARD_CONFIG_H_

#include <driver/gpio.h>

#define AUDIO_INPUT_SAMPLE_RATE  24000
#define AUDIO_OUTPUT_SAMPLE_RATE 24000

#define AUDIO_I2S_GPIO_MCLK GPIO_NUM_13
#define AUDIO_I2S_GPIO_WS   GPIO_NUM_10
#define AUDIO_I2S_GPIO_BCLK GPIO_NUM_12
#define AUDIO_I2S_GPIO_DIN  GPIO_NUM_11
#define AUDIO_I2S_GPIO_DOUT GPIO_NUM_9

#define AUDIO_CODEC_PA_PIN       GPIO_NUM_53
#define AUDIO_CODEC_I2C_SDA_PIN  GPIO_NUM_7
#define AUDIO_CODEC_I2C_SCL_PIN  GPIO_NUM_8
#define AUDIO_CODEC_ES8311_ADDR  ES8311_CODEC_DEFAULT_ADDR

#define BOOT_BUTTON_GPIO  GPIO_NUM_35

// JD9365D MIPI-DSI panel (Waveshare 10.1", 800x1280 portrait, 2 lanes).
// Will update when I got my hand on an actual touch-capable 480x480 display.
#define LCD_PANEL_H_RES            800
#define LCD_PANEL_V_RES            1280
#define LCD_MIPI_DSI_LANE_NUM      2
#define LCD_PIN_NUM_RST            GPIO_NUM_NC
#define MIPI_DSI_PHY_PWR_LDO_CHAN        3
#define MIPI_DSI_PHY_PWR_LDO_VOLTAGE_MV  2500

// The robot's spec display is 480x480, so the UI renders into a 480x480 window
// centred on the panel; the rest of the panel stays black.
#define DISPLAY_WIDTH     480
#define DISPLAY_HEIGHT    480
#define DISPLAY_OFFSET_X  ((LCD_PANEL_H_RES - DISPLAY_WIDTH) / 2)
#define DISPLAY_OFFSET_Y  ((LCD_PANEL_V_RES - DISPLAY_HEIGHT) / 2)
#define DISPLAY_SWAP_XY   false
#define DISPLAY_MIRROR_X  false
#define DISPLAY_MIRROR_Y  false

#endif // _BOARD_CONFIG_H_
