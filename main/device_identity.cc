#include "device_identity.h"

#ifdef CONFIG_DEVICE_JWT_AUTH

#include "settings.h"
#include "system_info.h"

#include <esp_log.h>
#include <esp_random.h>
#include <mbedtls/base64.h>
#include <mbedtls/pk.h>
#include <cstring>
#include <ctime>
#include <vector>

#define TAG "DeviceIdentity"

namespace {

constexpr char kNvsNamespace[] = "devauth";
constexpr char kNvsKey[] = "pk_der_b64";
// Contract constants from robo-worker src/auth/device.ts — do not drift.
constexpr char kAudience[] = "robo-worker/device";
constexpr int kJwtTtlSec = 120; // DEVICE_JWT_MAX_TTL_SEC
// Wall clock sanity floor (Sep 2020): anything below means settimeofday from
// the OTA server_time has not happened yet this boot.
constexpr time_t kSaneClockFloor = 1600000000;

std::string Base64Url(const uint8_t* data, size_t len) {
    size_t out_len = 0;
    // Query required size first (returns MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL).
    mbedtls_base64_encode(nullptr, 0, &out_len, data, len);
    std::vector<uint8_t> buf(out_len);
    if (mbedtls_base64_encode(buf.data(), buf.size(), &out_len, data, len) != 0) {
        return "";
    }
    std::string out(reinterpret_cast<char*>(buf.data()), out_len);
    for (auto& ch : out) {
        if (ch == '+') ch = '-';
        else if (ch == '/') ch = '_';
    }
    while (!out.empty() && out.back() == '=') out.pop_back();
    return out;
}

std::string Base64Std(const uint8_t* data, size_t len) {
    size_t out_len = 0;
    mbedtls_base64_encode(nullptr, 0, &out_len, data, len);
    std::vector<uint8_t> buf(out_len);
    if (mbedtls_base64_encode(buf.data(), buf.size(), &out_len, data, len) != 0) {
        return "";
    }
    return std::string(reinterpret_cast<char*>(buf.data()), out_len);
}

std::vector<uint8_t> Base64Decode(const std::string& in) {
    size_t out_len = 0;
    mbedtls_base64_decode(nullptr, 0, &out_len, reinterpret_cast<const uint8_t*>(in.data()), in.size());
    std::vector<uint8_t> buf(out_len);
    if (mbedtls_base64_decode(buf.data(), buf.size(), &out_len, reinterpret_cast<const uint8_t*>(in.data()),
                              in.size()) != 0) {
        return {};
    }
    buf.resize(out_len);
    return buf;
}

}  // namespace

DeviceIdentity& DeviceIdentity::GetInstance() {
    static DeviceIdentity instance;
    return instance;
}

bool DeviceIdentity::ClockLooksSane() {
    return time(nullptr) > kSaneClockFloor;
}

bool DeviceIdentity::EnsureKey() {
    std::lock_guard<std::mutex> lock(mutex_);
    return EnsureKeyLocked();
}

bool DeviceIdentity::EnsureKeyLocked() {
    if (key_ready_) return true;
    // psa_crypto_init() is done by ESP-IDF startup; calling again is harmless.
    if (psa_crypto_init() != PSA_SUCCESS) return false;
    if (LoadFromNvs() || GenerateAndPersist()) {
        key_ready_ = true;
        return true;
    }
    return false;
}

static void SetKeyAttrs(psa_key_attributes_t* attr) {
    psa_set_key_type(attr, PSA_KEY_TYPE_ECC_KEY_PAIR(PSA_ECC_FAMILY_SECP_R1));
    psa_set_key_bits(attr, 256);
    psa_set_key_usage_flags(attr, PSA_KEY_USAGE_SIGN_HASH | PSA_KEY_USAGE_SIGN_MESSAGE | PSA_KEY_USAGE_EXPORT);
    psa_set_key_algorithm(attr, PSA_ALG_ECDSA(PSA_ALG_SHA_256));
}

bool DeviceIdentity::LoadFromNvs() {
    Settings settings(kNvsNamespace, false);
    std::string der_b64 = settings.GetString(kNvsKey);
    if (der_b64.empty()) return false;
    auto der = Base64Decode(der_b64);
    if (der.empty()) {
        ESP_LOGW(TAG, "stored key is not valid base64 — regenerating");
        return false;
    }
    // DER from mbedtls_pk_write_key_der — the same NVS format the IDF 5.x
    // firmware wrote, so keys enrolled before the IDF 6 port keep working.
    mbedtls_pk_context pk;
    mbedtls_pk_init(&pk);
    int ret = mbedtls_pk_parse_key(&pk, der.data(), der.size(), nullptr, 0);
    if (ret == 0) {
        psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
        SetKeyAttrs(&attr);
        ret = mbedtls_pk_import_into_psa(&pk, &attr, &key_id_);
    }
    mbedtls_pk_free(&pk);
    if (ret != 0) {
        ESP_LOGW(TAG, "stored key failed to parse/import (-0x%04x) — regenerating", -ret);
        return false;
    }
    ESP_LOGI(TAG, "device identity key loaded from NVS");
    return true;
}

bool DeviceIdentity::GenerateAndPersist() {
    psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
    SetKeyAttrs(&attr);
    if (psa_generate_key(&attr, &key_id_) != PSA_SUCCESS) {
        ESP_LOGE(TAG, "psa_generate_key failed");
        return false;
    }
    mbedtls_pk_context pk;
    mbedtls_pk_init(&pk);
    uint8_t der[256];
    int ret = mbedtls_pk_copy_from_psa(key_id_, &pk);
    if (ret == 0) ret = mbedtls_pk_write_key_der(&pk, der, sizeof(der));
    mbedtls_pk_free(&pk);
    if (ret <= 0) {
        ESP_LOGE(TAG, "key DER export failed: -0x%04x", -ret);
        psa_destroy_key(key_id_);
        key_id_ = 0;
        return false;
    }
    std::string der_b64 = Base64Std(der + sizeof(der) - ret, ret);
    if (der_b64.empty()) return false;
    Settings settings(kNvsNamespace, true);
    settings.SetString(kNvsKey, der_b64);
    ESP_LOGI(TAG, "device identity key generated and persisted (first boot)");
    return true;
}

std::string DeviceIdentity::GetPublicJwkJson() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!EnsureKeyLocked()) return "";
    uint8_t point[65];  // 0x04 || X || Y
    size_t olen = 0;
    if (psa_export_public_key(key_id_, point, sizeof(point), &olen) != PSA_SUCCESS || olen != 65) {
        ESP_LOGE(TAG, "point export failed");
        return "";
    }
    std::string x = Base64Url(point + 1, 32);
    std::string y = Base64Url(point + 33, 32);
    if (x.empty() || y.empty()) return "";
    return std::string("{\"kty\":\"EC\",\"crv\":\"P-256\",\"x\":\"") + x + "\",\"y\":\"" + y + "\"}";
}

std::string DeviceIdentity::SignOtaJwt() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!EnsureKeyLocked()) return "";

    std::string device_id = SystemInfo::GetMacAddress();  // lowercase colon MAC = fleet Device-Id
    if (device_id.empty()) return "";

    // jti: 16 random bytes as 32 hex chars (well under the 128-char cap).
    uint8_t rnd[16];
    esp_fill_random(rnd, sizeof(rnd));
    char jti[33];
    for (int i = 0; i < 16; i++) snprintf(&jti[i * 2], 3, "%02x", rnd[i]);

    time_t now = time(nullptr);
    char payload[320];
    // sub == iss is enforced server-side; aud/ttl fixed by contract (§5.1).
    // %lu with an unsigned long cast, NOT %lld: printf without long long
    // support (newlib-nano, used before the IDF 6 port) emits the literal "ld"
    // and the server rejects the invalid JSON (hardware-verified). Unsigned
    // 32-bit epoch seconds are fine until 2106.
    snprintf(payload, sizeof(payload),
             "{\"sub\":\"%s\",\"iss\":\"%s\",\"aud\":\"%s\",\"jti\":\"%s\",\"iat\":%lu,\"exp\":%lu}",
             device_id.c_str(), device_id.c_str(), kAudience, jti, static_cast<unsigned long>(now),
             static_cast<unsigned long>(now + kJwtTtlSec));

    static const char kHeader[] = "{\"alg\":\"ES256\",\"typ\":\"JWT\"}";
    std::string signing_input =
        Base64Url(reinterpret_cast<const uint8_t*>(kHeader), sizeof(kHeader) - 1) + "." +
        Base64Url(reinterpret_cast<const uint8_t*>(payload), strlen(payload));

    // PSA ECDSA output is already the raw 64-byte r||s form WebCrypto verifies
    // (mbedtls_pk_sign would emit DER, which the server rejects).
    uint8_t sig[64];
    size_t sig_len = 0;
    if (psa_sign_message(key_id_, PSA_ALG_ECDSA(PSA_ALG_SHA_256),
                         reinterpret_cast<const uint8_t*>(signing_input.data()), signing_input.size(),
                         sig, sizeof(sig), &sig_len) != PSA_SUCCESS || sig_len != 64) {
        ESP_LOGE(TAG, "psa_sign_message failed");
        return "";
    }
    return signing_input + "." + Base64Url(sig, sizeof(sig));
}

#endif  // CONFIG_DEVICE_JWT_AUTH
