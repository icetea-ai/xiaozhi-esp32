# Port to ESP-IDF 6.0.2 (Tuni P4)

Date: 2026-09-25 · Status: approved, in progress on branch `idf6-port`

## Why

The Tuni P4 hardware uses ESP32-P4 v3.2 silicon. Bootloaders built with IDF 5.5.2 bootloop on it (a silent LP_WDT
reset), so the only working setup was an app built with 5.5.2 plus a bootloader built with 6.0.2, flashed separately.
A full image (`idf.py flash`, or the factory station's `merged-binary.bin`) bricked the board until it was reflashed.
Building everything with IDF 6.0.2 removes that split.

## Scope

- **In scope:** the Tuni P4 board (`tuni-p4` and `tuni-p4-p4x` variants) and all shared code it compiles: audio, protocols, BluFi and device identity.
- **Out of scope:** the other upstream boards. They may no longer compile on IDF 6; for example, `lilygo-t-display-p4` loses its
  `llgok/cpp_bus_driver` dependency. CI builds only the `tuni-p4*` variants.
- **IDF 6.0.2 only.** There is no dual 5.5/6.0 support, and the manifest requires `idf >= 6.0.2`.

## Changes

### Build and dependencies
- `main/idf_component.yml` component bumps:
  - Required by IDF 6: `espressif/cjson` (IDF 6 moved the bundled `json` component to the registry), esp-sr `~2.4.7`, esp_hosted `~2.12.13` (pulls esp_wifi_remote 1.6.x), esp_video `~2.0.1`,
    esp_emote_expression `^1.0.2`.
  - LCD and IO-expander drivers raised to their IDF 6-compatible majors.
- `sdkconfig.defaults`: `CONFIG_NEWLIB_NANO_FORMAT` removed. IDF 6 defaults to Picolibc, and that option would be silently ignored anyway.
- `Kconfig.projbuild` and `scripts/release.py`: the BluFi option no longer selects `MBEDTLS_DHM_C`, which no longer exists in Mbed TLS 4.
- Source fixes:
  - `I2S_NUM_x` instead of the removed `i2s_port_t` casts.
  - Explicit FreeRTOS includes where they used to arrive through driver headers.
  - A `_IO` macro guard around esp_video 2.x's `linux/ioctl.h`.

### Crypto (Mbed TLS 4 / PSA)
- **`device_identity`:** the P-256 key is a volatile PSA key, and signing uses `psa_sign_message(ECDSA(SHA-256))`, which returns raw r‖s.
  The NVS format is unchanged (base64 DER from `mbedtls_pk_write_key_der`). Keys enrolled under 5.x are loaded with
  `mbedtls_pk_parse_key` and `mbedtls_pk_import_into_psa`, so enrolled devices keep their identity.
- **MQTT UDP audio:** AES-128-CTR through `psa_cipher_*`, using the 16-byte packet header as the initial counter block.
  Output is byte-identical, so no server change is needed.
- **BluFi:** see the next section.

### BluFi protocol 0x04 (requires an app update)
IDF 6 raised the BluFi sub-version from 0x03 to 0x04 (the device now reports `BLUFI VERSION 0104`) and changed the security
negotiation. The firmware follows ESP-IDF's `examples/bluetooth/blufi/main/blufi_security.c`.

**App team change list:**

| | IDF 5.x (0x03) | IDF 6 (0x04) |
|---|---|---|
| Key exchange | DH over P/G supplied by the phone (1024-bit) | FFDH on the RFC 7919 **ffdhe3072** group. The phone must send those P and G, and a **384-byte** public key |
| PSK | MD5(shared secret), 16 bytes | **SHA-256**(shared secret), 32 bytes |
| Cipher | AES-128-**CFB128**, new IV per frame (iv[0] = frame sequence) | AES-**256-CTR**, one continuous counter stream per direction |
| IV | all-zero except iv[0] = sequence | phone→device: first 16 bytes of SHA-256("blufi_dec" ‖ secret); device→phone: first 16 bytes of SHA-256("blufi_enc" ‖ secret) |
| Checksum | CRC16-BE | unchanged |

The negotiation packet framing is unchanged: type `0x00` announces the length, and type `0x01` carries `len16|P|len16|G|len16|pubkey`.
The device replies with its 384-byte public key. Because each direction is one CTR stream, encrypted frames must be
processed in order and can't be skipped. The device ignores the P and G it receives and always uses ffdhe3072, so a phone key from any other group fails the
key agreement. Espressif's EspBlufi app releases that support IDF 6 are the reference client. Check which app
version added 0x04 before relying on it.

### C6 co-processor firmware
The host now runs esp_hosted 2.12.x, and the C6's slave firmware must be the matching esp_hosted 2.12.x release. Flash
it on the bench. Adding it to the factory station is a follow-up task.

### CI and tooling
- `.github/workflows/build.yml`: image `espressif/idf:v6.0.2`, with the variant matrix filtered to `tuni-p4*`.
- `scripts/factory/flash_station.py`: esptool v5 prints `Chip type:` / `Chip revision:` (it used to print `Chip is ...`) and uses dashed
  command names. `merged-binary.bin` now contains a 6.0.2 bootloader, so full-image flashing works on v3.2 silicon.

## Verification

No unit tests exist for this firmware, so verification is build plus bench:
1. Both `tuni-p4` variants build from clean on 6.0.2.
2. Bench test on a P4 v3.2 board, with a full image flashed at `0x0`:
   - It boots.
   - Wi-Fi connects through the C6.
   - The OTA check-in is accepted with an ES256 JWT, using a key from the old NVS where available.
   - MQTT and UDP voice work in both directions.
   - VAD and audio behave as before.
3. BluFi provisioning with a protocol-0x04 client.

## Risks
- The esp-sr 2.3 → 2.4 and esp_video 1.3 → 2.0 behaviour changes are untested beyond the bench checklist.
- Only the P4 target can be built locally, so other targets' breakage is known only from static review.
- BluFi provisioning with the current app is broken until the app ships its 0x04 support.
