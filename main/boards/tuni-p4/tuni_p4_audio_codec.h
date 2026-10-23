// main/boards/tuni-p4/tuni_p4_audio_codec.h
#ifndef _TUNI_P4_AUDIO_CODEC_H_
#define _TUNI_P4_AUDIO_CODEC_H_

#include "audio_codec.h"
#include "codecs/es8311_audio_codec.h"

#include <driver/gpio.h>
#include <mutex>

// Mic through the on-board ES8311 (I2S0), speakers through two external
// MAX98357A amps sharing one I2S bus (I2S1). The amps pick their channel by
// SD_MODE strapping (left: tied to 3V3, right: 220k to 3V3); the mono TTS
// stream is duplicated into both slots so each speaker plays it.
class TuniP4AudioCodec : public AudioCodec {
private:
    Es8311AudioCodec* mic_;
    std::mutex tx_mutex_;

    virtual int Read(int16_t* dest, int samples) override;
    virtual int Write(const int16_t* data, int samples) override;

public:
    TuniP4AudioCodec(Es8311AudioCodec* mic, int output_sample_rate,
        gpio_num_t spk_bclk, gpio_num_t spk_ws, gpio_num_t spk_dout);
    virtual ~TuniP4AudioCodec();

    virtual bool InputData(std::vector<int16_t>& data) override;
    virtual void SetInputGain(float gain) override;
    virtual void EnableInput(bool enable) override;
    virtual void EnableOutput(bool enable) override;
};

#endif // _TUNI_P4_AUDIO_CODEC_H_
