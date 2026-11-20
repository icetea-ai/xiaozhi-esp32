// main/boards/tuni-p4/tuni_p4_servo.h
#ifndef _TUNI_P4_SERVO_H_
#define _TUNI_P4_SERVO_H_

#include <driver/gpio.h>
#include <driver/ledc.h>

// One hobby servo (MG90S) driven by a 50 Hz LEDC PWM signal. The angle maps
// linearly onto [min_pulse_us, max_pulse_us]; the pulse keeps running after a
// move so the servo holds its position under load.
class TuniP4Servo {
private:
    ledc_channel_t channel_;
    int min_pulse_us_;
    int max_pulse_us_;
    int angle_ = -1;

public:
    TuniP4Servo(gpio_num_t gpio, ledc_timer_t timer, ledc_channel_t channel,
                int min_pulse_us, int max_pulse_us, int initial_angle);

    // Clamped to 0..180 degrees.
    void SetAngle(int angle);
    int GetAngle() const { return angle_; }
};

#endif // _TUNI_P4_SERVO_H_
