// main/boards/tuni-p4/config.h
#ifndef _BOARD_CONFIG_H_
#define _BOARD_CONFIG_H_

#include <driver/gpio.h>

#define AUDIO_INPUT_SAMPLE_RATE  24000
#define AUDIO_OUTPUT_SAMPLE_RATE 24000

// One full-duplex I2S bus (I2S1) for the external audio: two MAX98357A amps
// on DOUT and two ICS-43434 mics on DIN share BCLK and WS, so input and
// output must run at the same rate. GPIO20/21/22 are on the left header,
// next to a GND pin.
#define AUDIO_I2S_GPIO_BCLK  GPIO_NUM_21
#define AUDIO_I2S_GPIO_WS    GPIO_NUM_22
#define AUDIO_I2S_GPIO_DOUT  GPIO_NUM_20
#define AUDIO_I2S_GPIO_DIN   GPIO_NUM_23

// The on-board ES8311 is unused. Its speaker amp enable is held low, and its
// I2C bus is kept for the panel's power MCU.
#define AUDIO_CODEC_PA_PIN       GPIO_NUM_53
#define AUDIO_CODEC_I2C_SDA_PIN  GPIO_NUM_7
#define AUDIO_CODEC_I2C_SCL_PIN  GPIO_NUM_8

#define BOOT_BUTTON_GPIO  GPIO_NUM_35

// JD9365D MIPI-DSI panel (Waveshare 10.1", 800x1280 portrait, 2 lanes).
#define LCD_PANEL_H_RES            800
#define LCD_PANEL_V_RES            1280
#define LCD_MIPI_DSI_LANE_NUM      2
#define LCD_PIN_NUM_RST            GPIO_NUM_NC
#define MIPI_DSI_PHY_PWR_LDO_CHAN        3
#define MIPI_DSI_PHY_PWR_LDO_VOLTAGE_MV  2500

// The robot's spec display is 640x480, so the UI renders into a 640x480 window
// centred on the panel; the rest of the panel stays black.
#define DISPLAY_WIDTH     480
#define DISPLAY_HEIGHT    300
#define DISPLAY_OFFSET_X  ((LCD_PANEL_H_RES - DISPLAY_WIDTH) / 2)
#define DISPLAY_OFFSET_Y  ((LCD_PANEL_V_RES - DISPLAY_HEIGHT) / 2)
#define DISPLAY_SWAP_XY   false
#define DISPLAY_MIRROR_X  false
#define DISPLAY_MIRROR_Y  false

#endif // _BOARD_CONFIG_H_
