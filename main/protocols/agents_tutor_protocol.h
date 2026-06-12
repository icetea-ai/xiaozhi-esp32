#ifndef _AGENTS_TUTOR_PROTOCOL_H_
#define _AGENTS_TUTOR_PROTOCOL_H_

#include "protocol.h"

#include <web_socket.h>
#include <memory>
#include <string>

// Test/bring-up protocol for the robo-worker Tutor Durable Object FE endpoint:
//   wss://<host>/agents/tutor/child:<childId>?token=<token>
// See docs/esp32s3-agents-endpoint-guide.md (Cach B: PCM audio).
// This is NOT the production xiaozhi protocol: no "hello" handshake, no
// device auth. Wire shapes are translated to/from the internal
// tts/stt-style events that Application already understands.
class AgentsTutorProtocol : public Protocol {
public:
    AgentsTutorProtocol();
    ~AgentsTutorProtocol();

    bool Start() override;
    bool SendAudio(std::unique_ptr<AudioStreamPacket> packet) override;
    bool OpenAudioChannel() override;
    void CloseAudioChannel(bool send_goodbye = true) override;
    bool IsAudioChannelOpened() const override;

    void SendStartListening(ListeningMode mode) override;
    void SendStopListening() override;
    void SendWakeWordDetected(const std::string& wake_word) override;
    void SendAbortSpeaking(AbortReason reason) override;
    void SendListenAndSayMode() override;
    void SendFreeChatMode() override;

private:
    std::unique_ptr<WebSocket> websocket_;
    bool speaking_ = false;

    bool SendText(const std::string& text) override;
};

#endif  // _AGENTS_TUTOR_PROTOCOL_H_
