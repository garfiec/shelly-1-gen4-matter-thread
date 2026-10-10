//
// Copyright 2026 AUTOMATOUS.IO
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//


// shelly_wifi_password.cpp

#include "shelly_wifi_password.h"

#include <string.h>

#include <esp_log.h>
#include <nvs.h>

#include "shelly_setup_code.h"

static const char *TAG = "wifi_password";

static const char *NVS_NAMESPACE = "shelly_wifi";
static const char *NVS_KEY = "pass";

static bool load_own(char out[WIFI_PASSWORD_MAX + 1])
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    size_t len = WIFI_PASSWORD_MAX + 1;
    esp_err_t err = nvs_get_str(h, NVS_KEY, out, &len);
    nvs_close(h);
    if (err != ESP_OK || !wifi_password_valid(out)) {
        out[0] = '\0';
        return false;
    }
    return true;
}

shelly_wifi_security_t shelly_wifi_password_choose(char out[WIFI_PASSWORD_MAX + 1])
{
    out[0] = '\0';
    if (load_own(out)) {
        return SHELLY_WIFI_OWN;
    }
    setup_code_t st;
    char text[SETUP_CODE_FORMATTED_LEN];
    if (shelly_setup_code_sticker(&st) && setup_code_manual_format(st.passcode, st.discriminator, text) &&
        wifi_password_from_code_text(text, out)) {
        return SHELLY_WIFI_STICKER;
    }
    out[0] = '\0';
    return SHELLY_WIFI_OPEN;
}

bool shelly_wifi_password_save(const char *password)
{
    bool forget = password[0] == '\0';
    if (!forget && !wifi_password_valid(password)) {
        return false;
    }
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = forget ? nvs_erase_key(h, NVS_KEY) : nvs_set_str(h, NVS_KEY, password);
        if (err == ESP_ERR_NVS_NOT_FOUND) {
            err = ESP_OK;
        }
        if (err == ESP_OK) {
            err = nvs_commit(h);
        }
        nvs_close(h);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Saving the Wi-Fi mode password failed: %s", esp_err_to_name(err));
        return false;
    }
    ESP_LOGW(TAG, "%s; it applies the next time Wi-Fi mode starts",
             forget ? "Own Wi-Fi mode password forgotten" : "Own Wi-Fi mode password saved");
    return true;
}

void shelly_wifi_password_factory_reset(void)
{
    char own[WIFI_PASSWORD_MAX + 1];
    if (load_own(own) && shelly_wifi_password_save("")) {
        ESP_LOGW(TAG, "Factory reset: the Wi-Fi mode password is back to the sticker code, or none");
    }
}
