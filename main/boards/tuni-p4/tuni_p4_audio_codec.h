// main/boards/tuni-p4/tuni_p4_audio_codec.h
#ifndef _TUNI_P4_AUDIO_CODEC_H_
#define _TUNI_P4_AUDIO_CODEC_H_

#include "audio_codec.h"

#include <driver/gpio.h>
#include <driver/i2s_std.h>
#include <mutex>

// Two external MAX98357A amps and two ICS-43434 mics on one full-duplex I2S
// bus (I2S1), sharing BCLK and WS.
// - Amps pick their channel by SD_MODE strapping (left: tied to 3V3, right:
//   220k to 3V3); the mono TTS stream is duplicated into both slots.
// - Mics pick their slot by L/R (mic 1: GND = left, mic 2: 3V3 = right) and
//   share one SD line; the two channels are averaged into a mono stream.
class TuniP4AudioCodec : public AudioCodec {
private:
    std::mutex data_if_mutex_;

    virtual int Read(int16_t* dest, int samples) override;
    virtual int Write(const int16_t* data, int samples) override;

public:
    TuniP4AudioCodec(int input_sample_rate, int output_sample_rate,
        gpio_num_t bclk, gpio_num_t ws, gpio_num_t spk_dout, gpio_num_t mic_din);
    virtual ~TuniP4AudioCodec();

    virtual void EnableInput(bool enable) override;
    virtual void EnableOutput(bool enable) override;
};

#endif // _TUNI_P4_AUDIO_CODEC_H_
