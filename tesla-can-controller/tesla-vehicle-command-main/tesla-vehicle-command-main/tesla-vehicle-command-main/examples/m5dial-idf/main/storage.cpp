#include "storage.h"

#include "nvs_flash.h"
#include "nvs.h"
#include "esp_hmac.h"
#include "esp_partition.h"
#include "esp_log.h"

static const char *kNs = "tesla";

namespace storage {

    static esp_err_t nvs_get_str_alloc(nvs_handle_t h, const char *key, std::string &out) {
        size_t len = 0;
        esp_err_t err = nvs_get_str(h, key, nullptr, &len);
        if (err != ESP_OK) return err;
        std::string tmp; tmp.resize(len);
        err = nvs_get_str(h, key, tmp.data(), &len);
        if (err == ESP_OK) {
            if (len > 0 && tmp.back() == '\0') tmp.pop_back();
            out.swap(tmp);
        }
        return err;
    }

    static bool init_nvs_encryption() {
        // Check if NVS keys partition exists and initialize encryption
        const esp_partition_t* key_partition = esp_partition_find_first(
            ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_NVS_KEYS, NULL);
        
        if (key_partition == NULL) {
            ESP_LOGE("storage", "NVS keys partition not found");
            return false;
        }
        
        // Initialize NVS flash with encryption
        esp_err_t err = nvs_flash_secure_init_partition("nvs");
        if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
            // Erase and retry
            nvs_flash_erase_partition("nvs");
            err = nvs_flash_secure_init_partition("nvs");
        }
        
        return err == ESP_OK;
    }

    bool init() {
        // Initialize NVS with encryption support
        esp_err_t err = ESP_FAIL;
        
        // Try secure init first
        if (init_nvs_encryption()) {
            return true;
        }
        
        // Fallback to non-encrypted init if secure init fails
        ESP_LOGW("storage", "Secure NVS init failed, falling back to non-encrypted");
        err = nvs_flash_init();
        if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
            nvs_flash_erase();
            err = nvs_flash_init();
        }
        return err == ESP_OK;
    }

    bool loadVin(std::string &vin) {
        nvs_handle_t h; if (nvs_open(kNs, NVS_READONLY, &h) != ESP_OK) return false;
        esp_err_t err = nvs_get_str_alloc(h, "vin", vin);
        nvs_close(h); return err == ESP_OK && vin.size() == 17;
    }

    bool saveVin(const std::string &vin) {
        if (vin.size() != 17) return false;
        nvs_handle_t h; if (nvs_open(kNs, NVS_READWRITE, &h) != ESP_OK) return false;
        esp_err_t err = nvs_set_str(h, "vin", vin.c_str());
        if (err == ESP_OK) err = nvs_commit(h);
        nvs_close(h); return err == ESP_OK;
    }

    bool loadMac(std::string &mac) {
        nvs_handle_t h; if (nvs_open(kNs, NVS_READONLY, &h) != ESP_OK) return false;
        esp_err_t err = nvs_get_str_alloc(h, "mac", mac);
        nvs_close(h); return err == ESP_OK && mac.size() == 17;
    }

    bool saveMac(const std::string &mac) {
        if (mac.size() != 17) return false;
        nvs_handle_t h; if (nvs_open(kNs, NVS_READWRITE, &h) != ESP_OK) return false;
        esp_err_t err = nvs_set_str(h, "mac", mac.c_str());
        if (err == ESP_OK) err = nvs_commit(h);
        nvs_close(h); return err == ESP_OK;
    }

    bool hasKey() {
        nvs_handle_t h; if (nvs_open(kNs, NVS_READONLY, &h) != ESP_OK) return false;
        size_t len = 0; esp_err_t err = nvs_get_str(h, "key_pem", nullptr, &len);
        nvs_close(h); return err == ESP_OK && len > 0;
    }

    bool loadPrivateKey(std::string &pem) {
        nvs_handle_t h; if (nvs_open(kNs, NVS_READONLY, &h) != ESP_OK) return false;
        esp_err_t err = nvs_get_str_alloc(h, "key_pem", pem);
        nvs_close(h); return err == ESP_OK && !pem.empty();
    }

    bool savePrivateKey(const std::string &pem) {
        if (pem.empty()) return false;
        nvs_handle_t h; if (nvs_open(kNs, NVS_READWRITE, &h) != ESP_OK) return false;
        esp_err_t err = nvs_set_str(h, "key_pem", pem.c_str());
        if (err == ESP_OK) err = nvs_commit(h);
        nvs_close(h); return err == ESP_OK;
    }

    bool saveSessionInfo(int domain, const std::string &blob) {
        if (blob.empty()) return false;
        char key[24]; snprintf(key, sizeof key, "sess_%d", domain);
        nvs_handle_t h; if (nvs_open(kNs, NVS_READWRITE, &h) != ESP_OK) return false;
        esp_err_t err = nvs_set_blob(h, key, blob.data(), blob.size());
        if (err == ESP_OK) err = nvs_commit(h);
        nvs_close(h); return err == ESP_OK;
    }

    bool loadSessionInfo(int domain, std::string &blob) {
        char key[24]; snprintf(key, sizeof key, "sess_%d", domain);
        nvs_handle_t h; if (nvs_open(kNs, NVS_READONLY, &h) != ESP_OK) return false;
        size_t len = 0; esp_err_t err = nvs_get_blob(h, key, nullptr, &len);
        if (err != ESP_OK || len == 0) { nvs_close(h); return false; }
        blob.resize(len);
        err = nvs_get_blob(h, key, blob.data(), &len);
        if (err == ESP_OK && blob.size() != len) blob.resize(len);
        nvs_close(h);
        return err == ESP_OK && !blob.empty();
    }
}





