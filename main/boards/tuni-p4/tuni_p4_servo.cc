// main/boards/tuni-p4/tuni_p4_servo.cc
#include "tuni_p4_servo.h"

#include <esp_log.h>
#include <algorithm>

#define TAG "TuniP4Servo"

#define SERVO_PWM_FREQ_HZ    50
#define SERVO_PWM_PERIOD_US  (1000000 / SERVO_PWM_FREQ_HZ)
// 14 bits over a 20 ms period is ~1.2 us per step, well under the ~1 degree
// (~10 us) the MG90S can resolve.
#define SERVO_PWM_RESOLUTION LEDC_TIMER_14_BIT

TuniP4Servo::TuniP4Servo(gpio_num_t gpio, ledc_timer_t timer, ledc_channel_t channel,
                         int min_pulse_us, int max_pulse_us, int initial_angle)
    : channel_(channel), min_pulse_us_(min_pulse_us), max_pulse_us_(max_pulse_us) {
    ledc_timer_config_t timer_cfg = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = SERVO_PWM_RESOLUTION,
        .timer_num = timer,
        .freq_hz = SERVO_PWM_FREQ_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&timer_cfg));

    // Start at duty 0 (line low, servo unpowered by the signal) and set the
    // first angle only once the channel exists, so no stray pulse goes out.
    ledc_channel_config_t channel_cfg = {
        .gpio_num = gpio,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = channel,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = timer,
        .duty = 0,
        .hpoint = 0,
    };
    ESP_ERROR_CHECK(ledc_channel_config(&channel_cfg));

    SetAngle(initial_angle);
}

void TuniP4Servo::SetAngle(int angle) {
    angle = std::clamp(angle, 0, 180);
    uint32_t pulse_us = min_pulse_us_ + (max_pulse_us_ - min_pulse_us_) * angle / 180;
    uint32_t duty = pulse_us * ((1U << SERVO_PWM_RESOLUTION) - 1) / SERVO_PWM_PERIOD_US;
    ESP_ERROR_CHECK(ledc_set_duty(LEDC_LOW_SPEED_MODE, channel_, duty));
    ESP_ERROR_CHECK(ledc_update_duty(LEDC_LOW_SPEED_MODE, channel_));
    angle_ = angle;
    ESP_LOGI(TAG, "angle %d (%lu us)", angle, (unsigned long)pulse_us);
}
