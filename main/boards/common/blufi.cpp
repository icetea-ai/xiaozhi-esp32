#include "blufi.h"
#include <algorithm>
#include <cassert>
#include <cctype>
#include <cstring>
#include <memory>
#include <string>
#include <vector>
#include "esp_event.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/task.h"
#include "system_info.h"
#include "wifi_manager.h"

// True on boards with a local BT radio, where BLE runs on-chip and this class
// must drive the controller directly (esp_bt_controller_init/enable). False
// when BLE instead runs on an ESP-Hosted co-processor (e.g. ESP32-P4 host +
// ESP32-C6 slave) and Bluedroid talks to it over the hosted VHCI transport -
// see CONFIG_ESP_HOSTED_ENABLE_BT_BLUEDROID, which has no local controller to
// initialize and no esp_bt.h (controller API header) for its target.
#define BLUFI_USE_LOCAL_BT_CONTROLLER \
    ((CONFIG_BT_CONTROLLER_ENABLED || !CONFIG_BT_NIMBLE_ENABLED) && !CONFIG_ESP_HOSTED_ENABLE_BT_BLUEDROID)

#if BLUFI_USE_LOCAL_BT_CONTROLLER
#include "esp_bt.h"
#endif

#define BLUFI_DEVICE_NAME_PREFIX "TUNI_"

// BLE device name advertised during provisioning, e.g. "TUNI_A1B2C3" (last 3
// MAC bytes) - the TUNI app matches devices by this prefix (case-insensitive).
static std::string GetBlufiDeviceName() {
    auto mac = SystemInfo::GetMacAddress();
    std::string suffix;
    for (char c : mac) {
        if (c != ':') {
            suffix += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        }
    }
    if (suffix.size() > 6) {
        suffix = suffix.substr(suffix.size() - 6);
    }
    return std::string(BLUFI_DEVICE_NAME_PREFIX) + suffix;
}

#ifdef CONFIG_BT_BLUEDROID_ENABLED
#include "esp_bt_device.h"
#include "esp_bt_main.h"
#include "esp_gap_ble_api.h"
#endif

#if CONFIG_ESP_HOSTED_ENABLE_BT_BLUEDROID
#include "esp_bluedroid_hci.h"
// esp_hosted_bt.h (managed_components/espressif__esp_hosted) declares its C
// functions without an extern "C" guard, so a plain #include from this .cpp
// file would C++-mangle them and fail to link against the plain-C vhci_drv.c
// symbols. Force C linkage at the include site instead of patching the
// vendored header.
extern "C" {
#include "esp_hosted_bt.h"
#include "esp_hosted_misc.h"
}
#include "esp_hosted_bluedroid.h"  // hosted_hci_bluedroid_* (split out of esp_hosted_bt.h in 2.x)
#endif

#ifdef CONFIG_BT_NIMBLE_ENABLED
#include "console/console.h"
#include "host/ble_hs.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/gap/ble_svc_gap.h"
extern void esp_blufi_gatt_svr_register_cb(struct ble_gatt_register_ctxt* ctxt, void* arg);
extern int esp_blufi_gatt_svr_init(void);
extern void esp_blufi_gatt_svr_deinit(void);
extern void esp_blufi_btc_init(void);
extern void esp_blufi_btc_deinit(void);
#endif

extern "C" {
void esp_blufi_adv_start(void);

void esp_blufi_adv_stop(void);

void esp_blufi_disconnect(void);

void btc_blufi_report_error(esp_blufi_error_state_t state);

#ifdef CONFIG_BT_BLUEDROID_ENABLED
void esp_blufi_gap_event_handler(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t* param);
#endif

#ifdef CONFIG_BT_NIMBLE_ENABLED
void esp_blufi_gatt_svr_register_cb(struct ble_gatt_register_ctxt* ctxt, void* arg);
int esp_blufi_gatt_svr_init(void);
void esp_blufi_gatt_svr_deinit(void);
void esp_blufi_btc_init(void);
void esp_blufi_btc_deinit(void);
#endif
}

#include <wifi_station.h>
#include "esp_crc.h"
#include "esp_random.h"
#include "mbedtls/platform_util.h"
#include "ssid_manager.h"

static const char* BLUFI_TAG = "BLUFI_CLASS";

static wifi_mode_t GetWifiModeWithFallback(const WifiManager& wifi) {
    if (wifi.IsConfigMode()) {
        return WIFI_MODE_AP;
    }
    if (wifi.IsInitialized() && wifi.IsConnected()) {
        return WIFI_MODE_STA;
    }

    wifi_mode_t mode = WIFI_MODE_STA;
    esp_wifi_get_mode(&mode);
    return mode;
}

Blufi& Blufi::GetInstance() {
    static Blufi instance;
    return instance;
}

Blufi::Blufi()
    : m_sec(nullptr),
      m_ble_is_connected(false),
      m_sta_connected(false),
      m_sta_got_ip(false),
      m_provisioned(false),
      m_deinited(false),
      m_sta_ssid_len(0),
      m_sta_is_connecting(false) {
    memset(&m_sta_config, 0, sizeof(m_sta_config));
    memset(m_sta_bssid, 0, sizeof(m_sta_bssid));
    memset(m_sta_ssid, 0, sizeof(m_sta_ssid));
    memset(&m_sta_conn_info, 0, sizeof(m_sta_conn_info));
}

Blufi::~Blufi() {
    if (m_sec) {
        _security_deinit();
    }
}

esp_err_t Blufi::init() {
    esp_err_t ret = ESP_FAIL;
    inited_ = true;
    m_provisioned = false;
    m_deinited = false;

    // Start WiFi scan early to have results ready when user connects
    auto& wifi_manager = WifiManager::GetInstance();
    if (!wifi_manager.IsInitialized() || !wifi_manager.IsConfigMode()) {
        // start scan immediately
        start_wifi_scan();
    } else {
        ESP_LOGE(BLUFI_TAG,
                 "Blufi and WiFi hotspot network configuration cannot "
                 "be used simultaneously.");
        return ret;
    }

#if BLUFI_USE_LOCAL_BT_CONTROLLER
    ret = _controller_init();
    if (ret) {
        ESP_LOGE(BLUFI_TAG, "BLUFI controller init failed: %s", esp_err_to_name(ret));
        return ret;
    }
#endif

    ret = _host_and_cb_init();
    if (ret) {
        ESP_LOGE(BLUFI_TAG, "BLUFI host and cb init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(BLUFI_TAG, "BLUFI VERSION %04x", esp_blufi_get_version());
    return ESP_OK;
}

esp_err_t Blufi::deinit() {
    esp_err_t ret = ESP_OK;

    if (inited_) {
        if (m_deinited) {
            return ESP_OK;
        }
        m_deinited = true;
        if (m_scan_event_instance != nullptr) {
            esp_event_handler_instance_unregister(WIFI_EVENT, WIFI_EVENT_SCAN_DONE, m_scan_event_instance);
            m_scan_event_instance = nullptr;
        }
        ret = _host_deinit();
        if (ret) {
            ESP_LOGE(BLUFI_TAG, "Host deinit failed: %s", esp_err_to_name(ret));
        }
#if BLUFI_USE_LOCAL_BT_CONTROLLER
        ret = _controller_deinit();
        if (ret) {
            ESP_LOGE(BLUFI_TAG, "Controller deinit failed: %s", esp_err_to_name(ret));
        }
#endif
    }
    return ret;
}

#ifdef CONFIG_BT_BLUEDROID_ENABLED
esp_err_t Blufi::_host_init() {
#if CONFIG_ESP_HOSTED_ENABLE_BT_BLUEDROID
    // No local controller: bridge Bluedroid's HCI to the ESP-Hosted co-processor
    // over VHCI before esp_bluedroid_init() so the host stack has a transport.
    // Since esp_hosted 2.5.2 the co-processor's BT controller starts disabled
    // and must be brought up by the host first (else HCI_Reset times out).
    esp_err_t ctrl_ret = esp_hosted_bt_controller_init();
    if (ctrl_ret == ESP_OK) {
        ctrl_ret = esp_hosted_bt_controller_enable();
    }
    if (ctrl_ret != ESP_OK) {
        ESP_LOGE(BLUFI_TAG, "%s co-processor BT controller init failed: %s", __func__,
                 esp_err_to_name(ctrl_ret));
        return ESP_FAIL;
    }
    hosted_hci_bluedroid_open();
    static const esp_bluedroid_hci_driver_operations_t hosted_hci_ops = {
        .send = hosted_hci_bluedroid_send,
        .check_send_available = hosted_hci_bluedroid_check_send_available,
        .register_host_callback = hosted_hci_bluedroid_register_host_callback,
    };
    esp_err_t hci_ret = esp_bluedroid_attach_hci_driver(&hosted_hci_ops);
    if (hci_ret) {
        ESP_LOGE(BLUFI_TAG, "%s attach hosted HCI driver failed: %s", __func__,
                 esp_err_to_name(hci_ret));
        return ESP_FAIL;
    }
#endif
    esp_err_t ret = esp_bluedroid_init();
    if (ret) {
        ESP_LOGE(BLUFI_TAG, "%s init bluedroid failed: %s", __func__, esp_err_to_name(ret));
        return ESP_FAIL;
    }
    ret = esp_bluedroid_enable();
    if (ret) {
        ESP_LOGE(BLUFI_TAG, "%s enable bluedroid failed: %s", __func__, esp_err_to_name(ret));
        return ESP_FAIL;
    }
    ESP_LOGI(BLUFI_TAG, "BD ADDR: " ESP_BD_ADDR_STR, ESP_BD_ADDR_HEX(esp_bt_dev_get_address()));
    return ESP_OK;
}

esp_err_t Blufi::_host_deinit() {
    esp_err_t ret = esp_blufi_profile_deinit();
    if (ret != ESP_OK)
        return ret;

    ret = esp_bluedroid_disable();
    if (ret) {
        ESP_LOGE(BLUFI_TAG, "%s disable bluedroid failed: %s", __func__, esp_err_to_name(ret));
        return ESP_FAIL;
    }
    ret = esp_bluedroid_deinit();
    if (ret) {
        ESP_LOGE(BLUFI_TAG, "%s deinit bluedroid failed: %s", __func__, esp_err_to_name(ret));
        return ESP_FAIL;
    }
#if CONFIG_ESP_HOSTED_ENABLE_BT_BLUEDROID
    hosted_hci_bluedroid_close();
    esp_hosted_bt_controller_disable();
    esp_hosted_bt_controller_deinit(false);  // keep controller memory so BluFi can restart
#endif
    return ESP_OK;
}

esp_err_t Blufi::_gap_register_callback() {
    esp_err_t rc = esp_ble_gap_register_callback(esp_blufi_gap_event_handler);
    if (rc) {
        return rc;
    }
    return esp_blufi_profile_init();
}

esp_err_t Blufi::_host_and_cb_init() {
    static esp_blufi_callbacks_t blufi_callbacks = {
        .event_cb = &_event_callback_trampoline,
        .negotiate_data_handler = &_negotiate_data_handler_trampoline,
        .encrypt_func = &_encrypt_func_trampoline,
        .decrypt_func = &_decrypt_func_trampoline,
        .checksum_func = &_checksum_func_trampoline,
    };

    esp_err_t ret = _host_init();
    if (ret) {
        ESP_LOGE(BLUFI_TAG, "%s initialise host failed: %s", __func__, esp_err_to_name(ret));
        return ret;
    }
    ret = esp_blufi_register_callbacks(&blufi_callbacks);
    if (ret) {
        ESP_LOGE(BLUFI_TAG, "%s blufi register failed, error code = %x", __func__, ret);
        return ret;
    }
    ret = _gap_register_callback();
    if (ret) {
        ESP_LOGE(BLUFI_TAG, "%s gap register failed, error code = %x", __func__, ret);
        return ret;
    }
    return ESP_OK;
}
#endif /* CONFIG_BT_BLUEDROID_ENABLED */

#ifdef CONFIG_BT_NIMBLE_ENABLED
// Stubs for NimBLE specific store functionality
void ble_store_config_init();

void Blufi::_nimble_on_reset(int reason) {
    ESP_LOGE(BLUFI_TAG, "NimBLE Resetting state; reason=%d", reason);
}

void Blufi::_nimble_on_sync() { esp_blufi_profile_init(); }

void Blufi::_nimble_host_task(void* param) {
    ESP_LOGI(BLUFI_TAG, "BLE Host Task Started");
    nimble_port_run();
    nimble_port_freertos_deinit();
}

esp_err_t Blufi::_host_init() {
    ble_hs_cfg.reset_cb = _nimble_on_reset;
    ble_hs_cfg.sync_cb = _nimble_on_sync;
    ble_hs_cfg.gatts_register_cb = esp_blufi_gatt_svr_register_cb;

    ble_hs_cfg.sm_io_cap = 4;
#ifdef CONFIG_EXAMPLE_BONDING
    ble_hs_cfg.sm_bonding = 1;
#endif

    int rc = esp_blufi_gatt_svr_init();
    assert(rc == 0);

    ble_store_config_init();
    esp_blufi_btc_init();

    esp_err_t err = esp_nimble_enable(_nimble_host_task);
    if (err) {
        ESP_LOGE(BLUFI_TAG, "%s failed: %s", __func__, esp_err_to_name(err));
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t Blufi::_host_deinit(void) {
    esp_err_t ret = nimble_port_stop();
    if (ret == ESP_OK) {
        esp_nimble_deinit();
    }
    esp_blufi_gatt_svr_deinit();
    ret = esp_blufi_profile_deinit();
    esp_blufi_btc_deinit();
    return ret;
}

esp_err_t Blufi::_gap_register_callback(void) { return ESP_OK; }

esp_err_t Blufi::_host_and_cb_init() {
    static esp_blufi_callbacks_t blufi_callbacks = {
        .event_cb = &_event_callback_trampoline,
        .negotiate_data_handler = &_negotiate_data_handler_trampoline,
        .encrypt_func = &_encrypt_func_trampoline,
        .decrypt_func = &_decrypt_func_trampoline,
        .checksum_func = &_checksum_func_trampoline,
    };

    esp_err_t ret = esp_blufi_register_callbacks(&blufi_callbacks);
    if (ret) {
        ESP_LOGE(BLUFI_TAG, "%s blufi register failed, error code = %x", __func__, ret);
        return ret;
    }

    // Host init must be called after registering callbacks for NimBLE
    ret = _host_init();
    if (ret) {
        ESP_LOGE(BLUFI_TAG, "%s initialise host failed: %s", __func__, esp_err_to_name(ret));
        return ret;
    }
    return ESP_OK;
}
#endif /* CONFIG_BT_NIMBLE_ENABLED */

#if BLUFI_USE_LOCAL_BT_CONTROLLER
esp_err_t Blufi::_controller_init() {
    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    esp_err_t ret = esp_bt_controller_init(&bt_cfg);
    if (ret) {
        ESP_LOGE(BLUFI_TAG, "%s initialize controller failed: %s", __func__, esp_err_to_name(ret));
        return ret;
    }
    ret = esp_bt_controller_enable(ESP_BT_MODE_BLE);
    if (ret) {
        ESP_LOGE(BLUFI_TAG, "%s enable controller failed: %s", __func__, esp_err_to_name(ret));
        return ret;
    }

#ifdef CONFIG_BT_NIMBLE_ENABLED
    ret = esp_nimble_init();
    if (ret) {
        ESP_LOGE(BLUFI_TAG, "esp_nimble_init() failed: %s", esp_err_to_name(ret));
        return ret;
    }
#endif
    return ESP_OK;
}

esp_err_t Blufi::_controller_deinit() {
    esp_err_t ret = esp_bt_controller_disable();
    if (ret) {
        ESP_LOGE(BLUFI_TAG, "%s disable controller failed: %s", __func__, esp_err_to_name(ret));
    }
    ret = esp_bt_controller_deinit();
    if (ret) {
        ESP_LOGE(BLUFI_TAG, "%s deinit controller failed: %s", __func__, esp_err_to_name(ret));
    }
    return ret;
}
#endif

// Negotiation packet types (BluFi "negotiate data" sub-protocol).
#define SEC_TYPE_DH_PARAM_LEN 0x00
#define SEC_TYPE_DH_PARAM_DATA 0x01
#define DH_PARAM_LEN_MAX 1024  // bounds the phone-controlled malloc

void Blufi::_security_cleanup_aes() {
    psa_cipher_abort(&m_sec->enc_operation);
    psa_cipher_abort(&m_sec->dec_operation);
    psa_destroy_key(m_sec->aes_key);  // no-op for 0
    m_sec->aes_key = 0;
}

void Blufi::_security_cleanup_dh_param() {
    free(m_sec->dh_param);
    m_sec->dh_param = nullptr;
}

// IV for one direction: first 16 bytes of SHA-256(domain || shared secret).
static bool _derive_iv(const char* domain, const uint8_t* share_key, size_t share_len, uint8_t iv[16]) {
    psa_hash_operation_t op = PSA_HASH_OPERATION_INIT;
    uint8_t hash[32];
    size_t hash_len = 0;
    bool ok = psa_hash_setup(&op, PSA_ALG_SHA_256) == PSA_SUCCESS &&
              psa_hash_update(&op, (const uint8_t*)domain, strlen(domain)) == PSA_SUCCESS &&
              psa_hash_update(&op, share_key, share_len) == PSA_SUCCESS &&
              psa_hash_finish(&op, hash, sizeof(hash), &hash_len) == PSA_SUCCESS;
    if (!ok) {
        psa_hash_abort(&op);
    }
    memcpy(iv, hash, 16);
    mbedtls_platform_zeroize(hash, sizeof(hash));
    return ok;
}

static bool _start_ctr_stream(psa_cipher_operation_t* op, psa_key_id_t key, const uint8_t iv[16]) {
    // CTR is symmetric: the decrypt direction also uses an encrypt operation.
    return psa_cipher_encrypt_setup(op, key, PSA_ALG_CTR) == PSA_SUCCESS &&
           psa_cipher_set_iv(op, iv, 16) == PSA_SUCCESS;
}

void Blufi::_security_init() {
    m_sec = new BlufiSecurity();  // value-init: zeroed PSA operations are valid initial state
}

void Blufi::_security_deinit() {
    if (m_sec == nullptr)
        return;

    _security_cleanup_aes();
    _security_cleanup_dh_param();
    mbedtls_platform_zeroize(m_sec, sizeof(BlufiSecurity));
    delete m_sec;
    m_sec = nullptr;
}

void Blufi::_dh_negotiate_data_handler(uint8_t* data, int len, uint8_t** output_data,
                                       int* output_len, bool* need_free) {
    if (m_sec == nullptr) {
        ESP_LOGE(BLUFI_TAG, "Security not initialized in DH handler");
        btc_blufi_report_error(ESP_BLUFI_INIT_SECURITY_ERROR);
        return;
    }

    if (len < 1) {
        ESP_LOGE(BLUFI_TAG, "DH handler: data too short");
        btc_blufi_report_error(ESP_BLUFI_DATA_FORMAT_ERROR);
        return;
    }

    uint8_t type = data[0];
    switch (type) {
        case SEC_TYPE_DH_PARAM_LEN:
            if (len < 3) {
                ESP_LOGE(BLUFI_TAG, "DH_PARAM_LEN packet too short");
                btc_blufi_report_error(ESP_BLUFI_DATA_FORMAT_ERROR);
                return;
            }

            m_sec->dh_param_len = (data[1] << 8) | data[2];
            if (m_sec->dh_param_len == 0 || m_sec->dh_param_len > DH_PARAM_LEN_MAX) {
                ESP_LOGE(BLUFI_TAG, "Invalid DH param length %d", m_sec->dh_param_len);
                m_sec->dh_param_len = 0;
                btc_blufi_report_error(ESP_BLUFI_DH_PARAM_ERROR);
                return;
            }
            // A new negotiation replaces any previous session.
            _security_cleanup_dh_param();
            _security_cleanup_aes();
            m_sec->dh_param = (uint8_t*)malloc(m_sec->dh_param_len);
            if (m_sec->dh_param == nullptr) {
                ESP_LOGE(BLUFI_TAG, "DH malloc failed");
                m_sec->dh_param_len = 0;
                btc_blufi_report_error(ESP_BLUFI_DH_MALLOC_ERROR);
            }
            break;
        case SEC_TYPE_DH_PARAM_DATA: {
            if (m_sec->dh_param == nullptr) {
                ESP_LOGE(BLUFI_TAG, "DH param not allocated");
                btc_blufi_report_error(ESP_BLUFI_DH_PARAM_ERROR);
                return;
            }
            if (len < m_sec->dh_param_len + 1) {
                ESP_LOGE(BLUFI_TAG, "DH param data shorter than announced");
                btc_blufi_report_error(ESP_BLUFI_DH_PARAM_ERROR);
                return;
            }
            memcpy(m_sec->dh_param, &data[1], m_sec->dh_param_len);

            // Payload is len16|P|len16|G|len16|peer public key. P and G must be the
            // RFC 7919 ffdhe3072 group, which PSA uses implicitly, so skip them.
            const uint8_t* param = m_sec->dh_param;
            const uint8_t* end = param + m_sec->dh_param_len;
            const uint8_t* peer_pub = nullptr;
            size_t pub_len = 0;
            for (int field = 0; field < 3; field++) {
                if (end - param < 2) {
                    break;
                }
                size_t field_len = (param[0] << 8) | param[1];
                param += 2;
                if ((size_t)(end - param) < field_len) {
                    break;
                }
                if (field == 2) {
                    peer_pub = param;
                    pub_len = field_len;
                }
                param += field_len;
            }
            if (peer_pub == nullptr || pub_len != BlufiSecurity::kDhKeyLen) {
                ESP_LOGE(BLUFI_TAG, "Bad DH params (peer key %u bytes, need %u for ffdhe3072)",
                         (unsigned)pub_len, (unsigned)BlufiSecurity::kDhKeyLen);
                _security_cleanup_dh_param();
                btc_blufi_report_error(ESP_BLUFI_READ_PARAM_ERROR);
                return;
            }

            psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
            psa_set_key_type(&attr, PSA_KEY_TYPE_DH_KEY_PAIR(PSA_DH_FAMILY_RFC7919));
            psa_set_key_bits(&attr, BlufiSecurity::kDhKeyLen * 8);
            psa_set_key_algorithm(&attr, PSA_ALG_FFDH);
            psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_DERIVE);
            psa_key_id_t private_key = 0;
            size_t public_key_len = 0;
            psa_status_t status = psa_generate_key(&attr, &private_key);
            if (status == PSA_SUCCESS) {
                status = psa_export_public_key(private_key, m_sec->self_public_key,
                                               sizeof(m_sec->self_public_key), &public_key_len);
            }
            if (status != PSA_SUCCESS) {
                ESP_LOGE(BLUFI_TAG, "DH key generation failed: %d", (int)status);
                psa_destroy_key(private_key);
                _security_cleanup_dh_param();
                btc_blufi_report_error(ESP_BLUFI_MAKE_PUBLIC_ERROR);
                return;
            }
            status = psa_raw_key_agreement(PSA_ALG_FFDH, private_key, peer_pub, pub_len,
                                           m_sec->share_key, sizeof(m_sec->share_key),
                                           &m_sec->share_len);
            psa_destroy_key(private_key);
            _security_cleanup_dh_param();
            if (status != PSA_SUCCESS) {
                ESP_LOGE(BLUFI_TAG, "DH key agreement failed: %d", (int)status);
                btc_blufi_report_error(ESP_BLUFI_DH_PARAM_ERROR);
                return;
            }

            size_t hash_len = 0;
            if (psa_hash_compute(PSA_ALG_SHA_256, m_sec->share_key, m_sec->share_len, m_sec->psk,
                                 sizeof(m_sec->psk), &hash_len) != PSA_SUCCESS) {
                ESP_LOGE(BLUFI_TAG, "PSK derivation failed");
                btc_blufi_report_error(ESP_BLUFI_CALC_SHA_256_ERROR);
                return;
            }

            psa_key_attributes_t aes_attr = PSA_KEY_ATTRIBUTES_INIT;
            psa_set_key_type(&aes_attr, PSA_KEY_TYPE_AES);
            psa_set_key_bits(&aes_attr, sizeof(m_sec->psk) * 8);
            psa_set_key_algorithm(&aes_attr, PSA_ALG_CTR);
            psa_set_key_usage_flags(&aes_attr, PSA_KEY_USAGE_ENCRYPT | PSA_KEY_USAGE_DECRYPT);
            uint8_t iv_enc[16], iv_dec[16];
            bool ok = psa_import_key(&aes_attr, m_sec->psk, sizeof(m_sec->psk), &m_sec->aes_key) == PSA_SUCCESS &&
                      _derive_iv("blufi_enc", m_sec->share_key, m_sec->share_len, iv_enc) &&
                      _derive_iv("blufi_dec", m_sec->share_key, m_sec->share_len, iv_dec) &&
                      _start_ctr_stream(&m_sec->enc_operation, m_sec->aes_key, iv_enc) &&
                      _start_ctr_stream(&m_sec->dec_operation, m_sec->aes_key, iv_dec);
            mbedtls_platform_zeroize(iv_enc, sizeof(iv_enc));
            mbedtls_platform_zeroize(iv_dec, sizeof(iv_dec));
            mbedtls_platform_zeroize(m_sec->psk, sizeof(m_sec->psk));
            if (!ok) {
                ESP_LOGE(BLUFI_TAG, "AES session setup failed");
                _security_cleanup_aes();
                btc_blufi_report_error(ESP_BLUFI_ENCRYPT_ERROR);
                return;
            }

            *output_data = m_sec->self_public_key;
            *output_len = public_key_len;
            *need_free = false;
            ESP_LOGI(BLUFI_TAG, "DH negotiation completed successfully");
            break;
        }
        default:
            ESP_LOGE(BLUFI_TAG, "DH handler unknown type: %d", type);
            btc_blufi_report_error(ESP_BLUFI_DATA_FORMAT_ERROR);
    }
}

// Protocol 0x04 ignores iv8: each direction is one continuous CTR stream, so
// frames must be processed in order (BluFi's sequence numbers guarantee that).
static int _ctr_crypt(psa_cipher_operation_t* op, uint8_t* crypt_data, int crypt_len) {
    std::vector<uint8_t> out(crypt_len);
    size_t out_len = 0;
    if (psa_cipher_update(op, crypt_data, crypt_len, out.data(), out.size(), &out_len) != PSA_SUCCESS ||
        out_len != (size_t)crypt_len) {
        return -1;
    }
    memcpy(crypt_data, out.data(), out_len);
    return crypt_len;
}

int Blufi::_aes_encrypt(uint8_t iv8, uint8_t* crypt_data, int crypt_len) {
    if (!m_sec || m_sec->aes_key == 0 || !crypt_data || crypt_len <= 0) {
        ESP_LOGE(BLUFI_TAG, "Invalid parameters for AES encryption");
        return -ESP_ERR_INVALID_ARG;
    }
    int ret = _ctr_crypt(&m_sec->enc_operation, crypt_data, crypt_len);
    if (ret < 0) {
        ESP_LOGE(BLUFI_TAG, "AES encrypt failed");
    }
    return ret;
}

int Blufi::_aes_decrypt(uint8_t iv8, uint8_t* crypt_data, int crypt_len) {
    if (!m_sec || m_sec->aes_key == 0 || !crypt_data || crypt_len < 0) {
        ESP_LOGE(BLUFI_TAG, "Invalid parameters for AES decryption");
        return -ESP_ERR_INVALID_ARG;
    }
    if (crypt_len == 0) {
        return 0;
    }
    int ret = _ctr_crypt(&m_sec->dec_operation, crypt_data, crypt_len);
    if (ret < 0) {
        ESP_LOGE(BLUFI_TAG, "AES decrypt failed");
    }
    return ret;
}

uint16_t Blufi::_crc_checksum(uint8_t iv8, uint8_t* data, int len) {
    return esp_crc16_be(0, data, len);
}

int Blufi::_get_softap_conn_num() {
    auto& wifi = WifiManager::GetInstance();
    if (!wifi.IsInitialized() || !wifi.IsConfigMode()) {
        return 0;
    }

    wifi_sta_list_t sta_list{};
    if (esp_wifi_ap_get_sta_list(&sta_list) == ESP_OK) {
        return sta_list.num;
    }
    return 0;
}

bool Blufi::start_wifi_scan() {
    ESP_LOGI(BLUFI_TAG, "Starting dedicated WiFi scan");

    // Already running: caller can rely on the in-flight scan and await its done event.
    if (m_scan_in_progress) {
        ESP_LOGW(BLUFI_TAG, "Scan already in progress, skipping");
        return true;
    }

    m_scan_in_progress = true;

    // Subscribe once for every mode: the STA path (re-provisioning via BOOT
    // long-press while connected) needs the scan-done event as much as AP does.
    if (m_scan_event_instance == nullptr) {
        esp_event_handler_instance_register(WIFI_EVENT, WIFI_EVENT_SCAN_DONE,
                                            &Blufi::_wifi_scan_event_handler, this,
                                            &m_scan_event_instance);
    }

    // Get current WiFi mode
    wifi_mode_t current_mode;
    esp_err_t err = esp_wifi_get_mode(&current_mode);

    if (current_mode == WIFI_MODE_AP) {
        // If in AP mode, temporarily switch to APSTA to allow scanning
        ESP_LOGI(BLUFI_TAG, "WiFi in AP mode");
        err = esp_wifi_set_mode(WIFI_MODE_STA);
        if (err != ESP_OK) {
            ESP_LOGE(BLUFI_TAG, "Failed to set WiFi mode to STA: %s", esp_err_to_name(err));
            m_scan_in_progress = false;
            return false;
        }
        // Need to restart WiFi for mode change to take effect
        err = esp_wifi_start();
        if (err != ESP_OK) {
            ESP_LOGE(BLUFI_TAG, "Failed to start WiFi after mode switch: %s", esp_err_to_name(err));
            m_scan_in_progress = false;
            return false;
        }
        // Start scan
        err = esp_wifi_scan_start(NULL, false);
        if (err != ESP_OK) {
            ESP_LOGE(BLUFI_TAG, "Failed to start WiFi scan: %s", esp_err_to_name(err));
            m_scan_in_progress = false;
            return false;
        }
    } else if (current_mode == WIFI_MODE_STA || current_mode == WIFI_MODE_APSTA) {
        // Ensure WiFi driver is started (may have been stopped during config mode transition)
        err = esp_wifi_start();
        if (err != ESP_OK && err != ESP_ERR_WIFI_STATE) {
            ESP_LOGE(BLUFI_TAG, "Failed to start WiFi before scan: %s", esp_err_to_name(err));
            m_scan_in_progress = false;
            return false;
        }
        err = esp_wifi_scan_start(NULL, false);
        if (err != ESP_OK) {
            ESP_LOGE(BLUFI_TAG, "Failed to start WiFi scan: %s", esp_err_to_name(err));
            m_scan_in_progress = false;
            return false;
        }
    } else {
        ESP_LOGE(BLUFI_TAG, "Unexpected WiFi mode: %d", current_mode);
        m_scan_in_progress = false;
        return false;
    }

    ESP_LOGI(BLUFI_TAG, "WiFi scan started");
    return true;
}

void Blufi::_send_wifi_list() {
    if (m_ap_records.empty()) {
        ESP_LOGW(BLUFI_TAG, "No AP records available, sending WiFi scan fail");
        esp_blufi_send_error_info(ESP_BLUFI_WIFI_SCAN_FAIL);
        return;
    }

    ESP_LOGI(BLUFI_TAG, "Sending WiFi list with %d APs", m_ap_records.size());

    std::vector<esp_blufi_ap_record_t> blufi_ap_list;
    for (const auto& ap : m_ap_records) {
        esp_blufi_ap_record_t blufi_ap;
        memset(&blufi_ap, 0, sizeof(blufi_ap));
        memcpy(blufi_ap.ssid, ap.ssid, std::min((size_t)32, sizeof(ap.ssid)));
        blufi_ap.rssi = ap.rssi;
        blufi_ap_list.push_back(blufi_ap);
    }

    esp_blufi_send_wifi_list(blufi_ap_list.size(), blufi_ap_list.data());

    m_ap_records.clear();
    start_wifi_scan();
}

void Blufi::_wifi_scan_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id,
                                     void* event_data) {
    Blufi* self = static_cast<Blufi*>(arg);

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_SCAN_DONE) {
        ESP_LOGI(BLUFI_TAG, "WiFi scan done");

        uint16_t ap_num = 0;
        esp_wifi_scan_get_ap_num(&ap_num);

        if (ap_num == 0) {
            ESP_LOGW(BLUFI_TAG, "No APs found");
            self->m_ap_records.clear();
        } else {
            if (self->m_scan_should_save_ssid) {
                self->m_ap_records.resize(ap_num);
                esp_wifi_scan_get_ap_records(&ap_num, self->m_ap_records.data());

                ESP_LOGI(BLUFI_TAG, "Found %d APs", ap_num);
                for (const auto& ap : self->m_ap_records) {
                    ESP_LOGI(BLUFI_TAG, "  SSID: %s, RSSI: %d, Authmode: %d", (char*)ap.ssid,
                             ap.rssi, ap.authmode);
                }
            }
        }
        self->m_scan_in_progress = false;
        // Dispatch a pending GET_WIFI_LIST response if one is waiting on this scan.
        if (self->m_send_list_after_scan) {
            self->m_send_list_after_scan = false;
            self->_send_wifi_list();
        }
    }
}

void Blufi::_handle_event(esp_blufi_cb_event_t event, esp_blufi_cb_param_t* param) {
    switch (event) {
        case ESP_BLUFI_EVENT_INIT_FINISH:
            ESP_LOGI(BLUFI_TAG, "BLUFI init finish");
            esp_ble_gap_set_device_name(GetBlufiDeviceName().c_str());
            esp_blufi_adv_start();
            break;
        case ESP_BLUFI_EVENT_DEINIT_FINISH:
            ESP_LOGI(BLUFI_TAG, "BLUFI deinit finish");
            break;
        case ESP_BLUFI_EVENT_BLE_CONNECT:
            ESP_LOGI(BLUFI_TAG, "BLUFI ble connect");
            m_ble_is_connected = true;
            esp_blufi_adv_stop();
            _security_init();
            break;
        case ESP_BLUFI_EVENT_BLE_DISCONNECT:
            ESP_LOGI(BLUFI_TAG, "BLUFI ble disconnect");
            m_ble_is_connected = false;
            _security_deinit();
            if (!m_provisioned) {
                esp_blufi_adv_start();
            } else {
                esp_blufi_adv_stop();
                if (!m_deinited) {
                    xTaskCreate(
                        [](void* ctx) {
                            static_cast<Blufi*>(ctx)->deinit();
                            vTaskDelete(nullptr);
                        },
                        "blufi_deinit", 4096, this, 5, nullptr);
                }
            }
            break;
        case ESP_BLUFI_EVENT_SET_WIFI_OPMODE: {
            ESP_LOGI(BLUFI_TAG, "BLUFI Set WIFI opmode %d", param->wifi_mode.op_mode);
            auto& wifi_manager = WifiManager::GetInstance();
            if (!wifi_manager.IsInitialized() && !wifi_manager.Initialize()) {
                ESP_LOGE(BLUFI_TAG, "Failed to initialize WifiManager for opmode change");
                break;
            }
            switch (param->wifi_mode.op_mode) {
                case WIFI_MODE_STA:
                    wifi_manager.StartStation();
                    break;
                case WIFI_MODE_AP:
                    wifi_manager.StartConfigAp();
                    break;
                case WIFI_MODE_APSTA:
                    ESP_LOGW(BLUFI_TAG, "APSTA mode not supported, starting station only");
                    wifi_manager.StartStation();
                    break;
                default:
                    wifi_manager.StopStation();
                    wifi_manager.StopConfigAp();
                    break;
            }
            break;
        }
        case ESP_BLUFI_EVENT_REQ_CONNECT_TO_AP: {
            ESP_LOGI(BLUFI_TAG, "BLUFI request wifi connect to AP via esp-wifi-connect");
            std::string ssid(reinterpret_cast<const char*>(m_sta_config.sta.ssid));
            std::string password(reinterpret_cast<const char*>(m_sta_config.sta.password));

            m_scan_should_save_ssid = false;
            m_sta_ssid_len = static_cast<int>(std::min(ssid.size(), sizeof(m_sta_ssid)));
            memcpy(m_sta_ssid, ssid.c_str(), m_sta_ssid_len);
            memset(m_sta_bssid, 0, sizeof(m_sta_bssid));
            m_sta_connected = false;
            m_sta_got_ip = false;
            m_sta_is_connecting = true;
            m_sta_conn_info = {};
            m_sta_conn_info.sta_ssid = m_sta_ssid;
            m_sta_conn_info.sta_ssid_len = m_sta_ssid_len;

            // Everything below (NVS write + WiFi driver stop/init/restart) is slow -
            // hundreds of ms to a few seconds. Running it inline in this BLE
            // GATT-write callback blocked the Bluedroid/host BLE stack long enough
            // that the phone's own BLE supervision timeout fired and dropped the
            // link right after sending credentials, before the device ever got a
            // chance to report success/failure back over BLE (hardware-observed:
            // app log shows the native disconnect callback firing immediately after
            // the last credential write, not after the multi-second WiFi connect).
            // Move all of it into the background task, off the BLE callback path.
            struct ConnectTaskCtx {
                Blufi* self;
                std::string ssid;
                std::string password;
            };
            auto* task_ctx = new ConnectTaskCtx{this, std::move(ssid), std::move(password)};

            xTaskCreate(
                [](void* ctx_raw) {
                    std::unique_ptr<ConnectTaskCtx> owned_ctx(static_cast<ConnectTaskCtx*>(ctx_raw));
                    auto* self = owned_ctx->self;
                    auto& wifi = WifiManager::GetInstance();

                    SsidManager::GetInstance().AddSsid(owned_ctx->ssid, owned_ctx->password);

                    if (wifi.IsInitialized()) {
                        if (wifi.IsConfigMode()) {
                            wifi.StopConfigAp();
                        }
                        wifi.StopStation();
                    }

                    if (!wifi.IsInitialized() && !wifi.Initialize()) {
                        ESP_LOGE(BLUFI_TAG, "Failed to initialize WifiManager");
                        self->m_sta_is_connecting = false;
                        vTaskDelete(nullptr);
                        return;
                    }

                    vTaskDelay(pdMS_TO_TICKS(500));
                    wifi.StartStation();

                    // Stay comfortably under the app's end-to-end provisioning timeout
                    // (BluFiProvisioningService.ts: 60s) so a slow router (WPA retry, DHCP
                    // lease negotiation) gets a fair chance before we give up - 30s was still
                    // cutting connects off by a second or two on a multi-BSSID mesh network
                    // (scan-for-matching-AP alone can eat a big chunk of the budget), surfacing
                    // a premature "WiFi connection failed" to the user despite the ESP32
                    // actually connecting moments later. Leave ~10s margin under the app's
                    // 60s for the BLE round-trip + report handling.
                    constexpr int kConnectTimeoutMs = 50000;
                    constexpr TickType_t kDelayTick = pdMS_TO_TICKS(200);
                    int waited_ms = 0;

                    while (waited_ms < kConnectTimeoutMs && !wifi.IsConnected()) {
                        vTaskDelay(kDelayTick);
                        waited_ms += 200;
                    }

                    wifi_mode_t mode = GetWifiModeWithFallback(wifi);
                    const int softap_conn_num = _get_softap_conn_num();

                    if (wifi.IsConnected()) {
                        self->m_sta_is_connecting = false;
                        self->m_sta_connected = true;
                        self->m_sta_got_ip = true;
                        self->m_provisioned = true;

                        auto current_ssid = wifi.GetSsid();
                        if (!current_ssid.empty()) {
                            self->m_sta_ssid_len = static_cast<int>(
                                std::min(current_ssid.size(), sizeof(self->m_sta_ssid)));
                            memcpy(self->m_sta_ssid, current_ssid.c_str(), self->m_sta_ssid_len);
                        }

                        wifi_ap_record_t ap_info{};
                        if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
                            memcpy(self->m_sta_bssid, ap_info.bssid, sizeof(self->m_sta_bssid));
                        }

                        esp_blufi_extra_info_t info = {};
                        memcpy(info.sta_bssid, self->m_sta_bssid, sizeof(self->m_sta_bssid));
                        info.sta_bssid_set = true;
                        info.sta_ssid = self->m_sta_ssid;
                        info.sta_ssid_len = self->m_sta_ssid_len;
                        esp_blufi_send_wifi_conn_report(mode, ESP_BLUFI_STA_CONN_SUCCESS,
                                                        softap_conn_num, &info);
                        ESP_LOGI(BLUFI_TAG, "connected to WiFi");

                        // Don't proactively disconnect BLE here (unlike the upstream
                        // ESP-IDF blufi example, which only disconnects in response to
                        // ESP_BLUFI_EVENT_RECV_SLAVE_DISCONNECT_BLE - a client request).
                        // Calling esp_blufi_disconnect() immediately after queuing the
                        // success notification races the BLE stack: the link can tear
                        // down before the notification actually goes out over the air,
                        // so the app never sees the success report and spins until its
                        // own 60s provisioning timeout - even though the robot actually
                        // connected. The app (BluFiProvisioningService/useProvisioning)
                        // already disconnects BLE itself once provision() resolves, so
                        // the device doesn't need to force it.
                    } else {
                        self->m_sta_is_connecting = false;
                        self->m_sta_connected = false;
                        self->m_sta_got_ip = false;

                        esp_blufi_extra_info_t info = {};
                        info.sta_ssid = self->m_sta_ssid;
                        info.sta_ssid_len = self->m_sta_ssid_len;
                        esp_blufi_send_wifi_conn_report(mode, ESP_BLUFI_STA_CONN_FAIL,
                                                        softap_conn_num, &info);
                        ESP_LOGE(BLUFI_TAG, "Failed to connect to WiFi via esp-wifi-connect");
                    }
                    vTaskDelete(nullptr);
                },
                "blufi_wifi_conn", 4096, task_ctx, 5, nullptr);
            break;
        }
        case ESP_BLUFI_EVENT_REQ_DISCONNECT_FROM_AP:
            ESP_LOGI(BLUFI_TAG, "BLUFI request wifi disconnect from AP");
            if (WifiManager::GetInstance().IsInitialized()) {
                WifiManager::GetInstance().StopStation();
            }
            m_sta_is_connecting = false;
            m_sta_connected = false;
            m_sta_got_ip = false;
            break;
        case ESP_BLUFI_EVENT_GET_WIFI_STATUS: {
            auto& wifi = WifiManager::GetInstance();
            wifi_mode_t mode = GetWifiModeWithFallback(wifi);
            const int softap_conn_num = _get_softap_conn_num();

            if (wifi.IsInitialized() && wifi.IsConnected()) {
                m_sta_connected = true;
                m_sta_got_ip = true;

                auto current_ssid = wifi.GetSsid();
                if (!current_ssid.empty()) {
                    m_sta_ssid_len =
                        static_cast<int>(std::min(current_ssid.size(), sizeof(m_sta_ssid)));
                    memcpy(m_sta_ssid, current_ssid.c_str(), m_sta_ssid_len);
                }

                esp_blufi_extra_info_t info;
                memset(&info, 0, sizeof(esp_blufi_extra_info_t));
                memcpy(info.sta_bssid, m_sta_bssid, 6);
                info.sta_ssid = m_sta_ssid;
                info.sta_ssid_len = m_sta_ssid_len;
                esp_blufi_send_wifi_conn_report(mode, ESP_BLUFI_STA_CONN_SUCCESS, softap_conn_num,
                                                &info);
            } else if (m_sta_is_connecting) {
                esp_blufi_send_wifi_conn_report(mode, ESP_BLUFI_STA_CONNECTING, softap_conn_num,
                                                &m_sta_conn_info);
            } else {
                esp_blufi_send_wifi_conn_report(mode, ESP_BLUFI_STA_CONN_FAIL, softap_conn_num,
                                                &m_sta_conn_info);
            }
            ESP_LOGI(BLUFI_TAG, "BLUFI get wifi status");
            break;
        }
        case ESP_BLUFI_EVENT_RECV_STA_BSSID:
            memcpy(m_sta_config.sta.bssid, param->sta_bssid.bssid, 6);
            m_sta_config.sta.bssid_set = true;
            ESP_LOGI(BLUFI_TAG, "Recv STA BSSID");
            break;
        case ESP_BLUFI_EVENT_RECV_STA_SSID:
            strncpy((char*)m_sta_config.sta.ssid, (char*)param->sta_ssid.ssid,
                    param->sta_ssid.ssid_len);
            m_sta_config.sta.ssid[param->sta_ssid.ssid_len] = '\0';
            ESP_LOGI(BLUFI_TAG, "Recv STA SSID: %s", m_sta_config.sta.ssid);
            break;
        case ESP_BLUFI_EVENT_RECV_STA_PASSWD:
            strncpy((char*)m_sta_config.sta.password, (char*)param->sta_passwd.passwd,
                    param->sta_passwd.passwd_len);
            m_sta_config.sta.password[param->sta_passwd.passwd_len] = '\0';
            ESP_LOGI(BLUFI_TAG, "Recv STA PASSWORD (%d bytes)", param->sta_passwd.passwd_len);
            break;
        case ESP_BLUFI_EVENT_GET_WIFI_LIST: {
            ESP_LOGI(BLUFI_TAG, "BLUFI get wifi list");
            // Case 1: a scan is already in flight (init scan or refresh scan started by
            // the previous _send_wifi_list()). Defer the response to its done handler
            // instead of blocking the BluFi task.
            if (m_scan_in_progress) {
                m_send_list_after_scan = true;
                break;
            }
            // Case 2: cache is populated. Respond immediately; _send_wifi_list() also
            // kicks off an async refresh scan to keep the cache fresh.
            if (!m_ap_records.empty()) {
                _send_wifi_list();
                break;
            }
            // Case 3: no cache (e.g. driver was stopped during a config-mode transition,
            // init scan never completed). Trigger a real scan and dispatch from the
            // scan-done handler. If the scan cannot start, return an error frame so the
            // App exits its wait state instead of timing out.
            m_scan_should_save_ssid = true;
            m_send_list_after_scan = true;
            if (!start_wifi_scan()) {
                m_send_list_after_scan = false;
                esp_blufi_send_error_info(ESP_BLUFI_WIFI_SCAN_FAIL);
            }
            break;
        }
        default:
            ESP_LOGW(BLUFI_TAG, "Unhandled event: %d", event);
            break;
    }
}

void Blufi::_event_callback_trampoline(esp_blufi_cb_event_t event, esp_blufi_cb_param_t* param) {
    GetInstance()._handle_event(event, param);
}

void Blufi::_negotiate_data_handler_trampoline(uint8_t* data, int len, uint8_t** output_data,
                                               int* output_len, bool* need_free) {
    GetInstance()._dh_negotiate_data_handler(data, len, output_data, output_len, need_free);
}

int Blufi::_encrypt_func_trampoline(uint8_t iv8, uint8_t* crypt_data, int crypt_len) {
    return GetInstance()._aes_encrypt(iv8, crypt_data, crypt_len);
}

int Blufi::_decrypt_func_trampoline(uint8_t iv8, uint8_t* crypt_data, int crypt_len) {
    return GetInstance()._aes_decrypt(iv8, crypt_data, crypt_len);
}

uint16_t Blufi::_checksum_func_trampoline(uint8_t iv8, uint8_t* data, int len) {
    return _crc_checksum(iv8, data, len);
}
