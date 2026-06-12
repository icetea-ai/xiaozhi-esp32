#include "agents_tutor_protocol.h"
#include "board.h"
#include "system_info.h"
#include "settings.h"

#include <algorithm>
#include <cstring>
#include <cJSON.h>
#include <esp_log.h>

#define TAG "AgentsTutor"

// These Kconfig values only end up in sdkconfig.h when
// CONFIG_CONNECT_TO_AGENTS_TUTOR_TEST is enabled, but this file is always
// compiled, so provide fallbacks.
#ifndef CONFIG_AGENTS_TUTOR_WS_URL
#define CONFIG_AGENTS_TUTOR_WS_URL "wss://robo-worker.taskfi.workers.dev/agents/tutor/"
#endif
#ifndef CONFIG_AGENTS_TUTOR_TOKEN
#define CONFIG_AGENTS_TUTOR_TOKEN "test"
#endif

// cf_agent_state frames can be several KB; a small receive buffer fragments
// them and breaks cJSON parsing. We ignore those frames anyway, but the
// buffer must be large enough for esp_websocket to hand us the whole thing.
#define AGENTS_TUTOR_RECEIVE_BUFFER_SIZE 16384

AgentsTutorProtocol::AgentsTutorProtocol() {
    server_audio_format_ = kAudioFormatPcm;
    server_sample_rate_ = 24000;
    server_frame_duration_ = 40;
}

AgentsTutorProtocol::~AgentsTutorProtocol() {
}

bool AgentsTutorProtocol::Start() {
    return true;
}

bool AgentsTutorProtocol::SendAudio(std::unique_ptr<AudioStreamPacket> packet) {
    if (websocket_ == nullptr || !websocket_->IsConnected()) {
        return false;
    }
    // Cach B upstream: raw PCM int16 LE 24kHz mono, no framing wrapper.
    return websocket_->Send(packet->payload.data(), packet->payload.size(), true);
}

bool AgentsTutorProtocol::SendText(const std::string& text) {
    if (websocket_ == nullptr || !websocket_->IsConnected()) {
        return false;
    }
    if (!websocket_->Send(text)) {
        ESP_LOGE(TAG, "Failed to send text: %s", text.c_str());
        return false;
    }
    return true;
}

void AgentsTutorProtocol::SendStartListening(ListeningMode mode) {
    // No "start listening" frame in this protocol: the device may stream
    // mic audio at any time, the server VAD/STT just consumes it.
}

void AgentsTutorProtocol::SendStopListening() {
    // VAD detected end-of-speech: flush STT on the server.
    SendText("{\"type\":\"eos\"}");
}

void AgentsTutorProtocol::SendWakeWordDetected(const std::string& wake_word) {
    // Not supported by this endpoint.
}

void AgentsTutorProtocol::SendAbortSpeaking(AbortReason reason) {
    // Not supported by this endpoint.
}

bool AgentsTutorProtocol::IsAudioChannelOpened() const {
    return websocket_ != nullptr && websocket_->IsConnected() && !error_occurred_ && !IsTimeout();
}

void AgentsTutorProtocol::CloseAudioChannel(bool send_goodbye) {
    (void)send_goodbye;
    websocket_.reset();
}

bool AgentsTutorProtocol::OpenAudioChannel() {
    error_occurred_ = false;
    speaking_ = false;

    Settings settings("agents_tutor", false);
    std::string base_url = settings.GetString("url");
    if (base_url.empty()) {
        base_url = CONFIG_AGENTS_TUTOR_WS_URL;
    }
    std::string token = settings.GetString("token");
    if (token.empty()) {
        token = CONFIG_AGENTS_TUTOR_TOKEN;
    }
    std::string child_id = settings.GetString("child_id");
    if (child_id.empty()) {
        std::string mac = SystemInfo::GetMacAddress();
        mac.erase(std::remove(mac.begin(), mac.end(), ':'), mac.end());
        child_id = "esp32-" + mac;
    }

    auto network = Board::GetInstance().GetNetwork();
    websocket_ = network->CreateWebSocket(1);
    if (websocket_ == nullptr) {
        ESP_LOGE(TAG, "Failed to create websocket");
        return false;
    }
    websocket_->SetReceiveBufferSize(AGENTS_TUTOR_RECEIVE_BUFFER_SIZE);

    websocket_->OnData([this](const char* data, size_t len, bool binary) {
        last_incoming_time_ = std::chrono::steady_clock::now();

        if (binary) {
            // Cach B downstream: raw PCM int16 LE 24kHz mono, write straight to I2S.
            if (!speaking_) {
                speaking_ = true;
                cJSON* tts = cJSON_CreateObject();
                cJSON_AddStringToObject(tts, "type", "tts");
                cJSON_AddStringToObject(tts, "state", "start");
                if (on_incoming_json_ != nullptr) {
                    on_incoming_json_(tts);
                }
                cJSON_Delete(tts);
            }
            if (on_incoming_audio_ != nullptr) {
                on_incoming_audio_(std::make_unique<AudioStreamPacket>(AudioStreamPacket{
                    .format = kAudioFormatPcm,
                    .sample_rate = server_sample_rate_,
                    .frame_duration = server_frame_duration_,
                    .timestamp = 0,
                    .payload = std::vector<uint8_t>((uint8_t*)data, (uint8_t*)data + len)
                }));
            }
            return;
        }

        auto root = cJSON_ParseWithLength(data, len);
        if (root == nullptr) {
            return;
        }

        // Golden rule: frames without an "event" field are Agents SDK
        // bookkeeping (cf_agent_identity/state/mcp_servers) and are ignored.
        auto event = cJSON_GetObjectItem(root, "event");
        if (!cJSON_IsString(event)) {
            cJSON_Delete(root);
            return;
        }

        if (strcmp(event->valuestring, "chunk") == 0) {
            auto text = cJSON_GetObjectItem(root, "text");
            if (cJSON_IsString(text) && on_incoming_json_ != nullptr) {
                cJSON* sentence = cJSON_CreateObject();
                cJSON_AddStringToObject(sentence, "type", "tts");
                cJSON_AddStringToObject(sentence, "state", "sentence_start");
                cJSON_AddStringToObject(sentence, "text", text->valuestring);
                on_incoming_json_(sentence);
                cJSON_Delete(sentence);
            }
        } else if (strcmp(event->valuestring, "done") == 0) {
            if (speaking_) {
                speaking_ = false;
                if (on_incoming_json_ != nullptr) {
                    cJSON* tts = cJSON_CreateObject();
                    cJSON_AddStringToObject(tts, "type", "tts");
                    cJSON_AddStringToObject(tts, "state", "stop");
                    on_incoming_json_(tts);
                    cJSON_Delete(tts);
                }
            }
            // Mandatory between turns: without this the intent dispatcher
            // silently drops every subsequent message.
            SendText("{\"type\":\"playback_drained\"}");
        }
        // chunk_boundary / trace / anything else -> ignore

        cJSON_Delete(root);
    });

    websocket_->OnDisconnected([this]() {
        ESP_LOGI(TAG, "Agents Tutor websocket disconnected");
        if (on_audio_channel_closed_ != nullptr) {
            on_audio_channel_closed_();
        }
    });

    std::string url = base_url + "child:" + child_id + "?token=" + token;
    ESP_LOGI(TAG, "Connecting to Agents Tutor test endpoint: %s", url.c_str());
    if (!websocket_->Connect(url.c_str())) {
        ESP_LOGE(TAG, "Failed to connect to Agents Tutor websocket, code=%d", websocket_->GetLastError());
        SetError("Failed to connect to Agents Tutor test server");
        return false;
    }

    // IsTimeout() compares against last_incoming_time_, which OnData updates;
    // seed it now since this protocol has no hello handshake to do that for us.
    last_incoming_time_ = std::chrono::steady_clock::now();

    if (on_audio_channel_opened_ != nullptr) {
        on_audio_channel_opened_();
    }

    return true;
}
