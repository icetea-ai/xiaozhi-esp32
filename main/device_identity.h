#ifndef DEVICE_IDENTITY_H
#define DEVICE_IDENTITY_H

#include <sdkconfig.h>

#ifdef CONFIG_DEVICE_JWT_AUTH

#include <psa/crypto.h>
#include <mutex>
#include <string>

// Per-device identity key + ES256 OTA JWTs (robo-worker factory-provisioning
// spec 2026-07-06 §5). The P-256 private key is generated on the device on
// first boot, persisted in NVS, and NEVER leaves the device — only the public
// JWK is exported (FACTORY boot line) for fleet enrollment.
class DeviceIdentity {
public:
    static DeviceIdentity& GetInstance();

    // Load the key from NVS or generate + persist it. Lazy, mutexed,
    // idempotent. False = crypto/NVS failure (caller degrades to
    // unauthenticated operation rather than blocking boot).
    bool EnsureKey();

    // {"kty":"EC","crv":"P-256","x":"<b64url>","y":"<b64url>"} or "" on failure.
    std::string GetPublicJwkJson();

    // Short-lived ES256 JWT for the OTA check-in, per the server contract
    // (src/auth/device.ts): sub = iss = Device-Id, aud robo-worker/device,
    // random jti, exp = iat + 120s, raw r||s signature. "" on failure.
    std::string SignOtaJwt();

    // The JWT is only verifiable with a sane wall clock (iat/exp). The clock
    // is set from the OTA response's server_time, so the first request of a
    // boot may predate it — see the bootstrap retry in ota.cc.
    static bool ClockLooksSane();

private:
    DeviceIdentity() = default;
    bool EnsureKeyLocked();
    bool LoadFromNvs();
    bool GenerateAndPersist();

    std::mutex mutex_;
    bool key_ready_ = false;
    psa_key_id_t key_id_ = 0;  // volatile PSA key; the DER copy in NVS is the persistent form
};

#endif // CONFIG_DEVICE_JWT_AUTH

#endif // DEVICE_IDENTITY_H
