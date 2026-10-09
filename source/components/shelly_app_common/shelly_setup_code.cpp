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

// shelly_setup_code.cpp

#include "shelly_setup_code.h"
#include "shelly_settings.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include <cJSON.h>
#include <esp_log.h>
#include <esp_partition.h>
#include <esp_random.h>
#include <nvs.h>
#include <sdkconfig.h>

#include <crypto/CHIPCryptoPAL.h>
#include <esp_matter_providers.h>
#include <platform/CommissionableDataProvider.h>
#include <platform/PlatformManager.h>

// Without the custom provider, CHIP quietly serves its test code instead
// (CommissionableDataProvider.cpp): make that a build error.
#if !CONFIG_CUSTOM_COMMISSIONABLE_DATA_PROVIDER
#error "Set CONFIG_CUSTOM_COMMISSIONABLE_DATA_PROVIDER=y (sdkconfig.defaults): the device's own setup code needs it"
#endif

#if CONFIG_SHELLY_MAINTENANCE
#define OWN_CODE 1
#else
#define OWN_CODE 0 // No Wi-Fi page to show or set a code: the sticker's or the test code
#endif

static const char *TAG = "setup_code";

static const char *NVS_NAMESPACE = "shelly_code";
static const char *NVS_KEY = "code";

// Guards the saved record and the staged change (the provider runs on the
// Matter thread; the maintenance page and the button on their own tasks).
static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;

static uint32_t rand32(void *)
{
    return esp_random();
}

// ---- Shelly's sticker code, from the stock "shelly" partition ----
// Read once, only read: never written. Only the three setup code fields are
// used; the partition's keys and certificates are never looked at.

#define SHELLY_PARTITION_SUBTYPE 0x88
#define SHELLY_PARTITION_READ_MAX 8192 // Its JSON is about 2.6 KB

static setup_code_sticker_t s_sticker;
static int8_t s_sticker_state; // 0 not read yet, 1 usable, -1 none

static const char *json_string(const cJSON *obj, const char *key)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsString(v) ? v->valuestring : nullptr;
}

static bool read_sticker(setup_code_sticker_t *out)
{
    const esp_partition_t *part = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, (esp_partition_subtype_t)SHELLY_PARTITION_SUBTYPE, "shelly");
    if (part == nullptr) {
        ESP_LOGW(TAG, "No Shelly partition: no sticker code");
        return false;
    }
    size_t len = part->size < SHELLY_PARTITION_READ_MAX ? part->size : SHELLY_PARTITION_READ_MAX;
    char *buf = (char *)malloc(len);
    if (buf == nullptr) {
        ESP_LOGW(TAG, "No memory to read the Shelly partition");
        return false;
    }
    bool ok = false;
    esp_err_t err = esp_partition_read(part, 0, buf, len);
    if (err == ESP_OK) {
        // The JSON ends at the first erased (0xFF) or NUL byte.
        size_t n = 0;
        while (n < len && buf[n] != (char)0xFF && buf[n] != '\0') {
            n++;
        }
        cJSON *root = n > 0 && n < len ? cJSON_ParseWithLength(buf, n) : nullptr;
        const cJSON *matter = cJSON_GetObjectItemCaseSensitive(root, "matter");
        ok = setup_code_sticker_parse(json_string(matter, "setup_qr_code"), json_string(matter, "setup_manual_code"),
                                      json_string(matter, "setup_verifier"), out);
        cJSON_Delete(root);
    }
    memset(buf, 0, len); // It also held the device's keys
    free(buf);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Reading the Shelly partition failed: %s", esp_err_to_name(err));
    } else if (!ok) {
        ESP_LOGW(TAG, "No usable sticker code in the Shelly partition");
    }
    return ok;
}

// The sticker code, or nullptr. With s_lock held.
static const setup_code_sticker_t *sticker(void)
{
    if (s_sticker_state == 0) {
        s_sticker_state = read_sticker(&s_sticker) ? 1 : -1;
    }
    return s_sticker_state > 0 ? &s_sticker : nullptr;
}

#if OWN_CODE
static bool load(setup_code_t *out)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    uint8_t rec[SETUP_CODE_RECORD_LEN];
    size_t len = sizeof(rec);
    esp_err_t err = nvs_get_blob(h, NVS_KEY, rec, &len);
    nvs_close(h);
    if (err != ESP_OK) {
        return false;
    }
    if (!setup_code_record_unpack(rec, len, out)) {
        ESP_LOGW(TAG, "Saved setup code is not valid: making a new one");
        return false;
    }
    return true;
}

#endif

#if OWN_CODE
// Forget the owner's own code, keeping a generated one (the fallback for
// a device without a sticker code). With s_lock held.
static bool forget_user_code(void)
{
    setup_code_t saved;
    if (!load(&saved) || saved.source != SETUP_CODE_SOURCE_USER) {
        return true;
    }
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = nvs_erase_key(h, NVS_KEY);
        if (err == ESP_OK) {
            err = nvs_commit(h);
        }
        nvs_close(h);
    }
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGE(TAG, "Forgetting your setup code failed: %s", esp_err_to_name(err));
        return false;
    }
    return true;
}
#endif

static bool save(const setup_code_t *code)
{
#if OWN_CODE
    if (code->source == SETUP_CODE_SOURCE_STICKER) {
        return forget_user_code(); // The sticker code is read, never saved
    }
#endif
    uint8_t rec[SETUP_CODE_RECORD_LEN];
    setup_code_record_pack(code, rec);
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = nvs_set_blob(h, NVS_KEY, rec, sizeof(rec));
        if (err == ESP_OK) {
            err = nvs_commit(h);
        }
        nvs_close(h);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Saving the setup code failed: %s", esp_err_to_name(err));
        return false;
    }
    return true;
}

static void test_code(setup_code_t *out)
{
    out->passcode = SETUP_CODE_TEST_PASSCODE;
    out->discriminator = SETUP_CODE_TEST_DISCRIMINATOR;
    out->source = SETUP_CODE_SOURCE_TEST;
}

// The code to pair with, first match wins: the owner's own, the sticker's,
// a generated one (made and saved now if there is none), the test code.
// False only when it falls back to the test code. Call with s_lock held, and
// only once a radio is on: esp_random() is only truly random then.
static bool resolve(setup_code_t *out, bool *created)
{
    *created = false;
#if OWN_CODE
    setup_code_t saved;
    bool have = load(&saved);
    if (have && saved.source == SETUP_CODE_SOURCE_USER) {
        *out = saved;
        return true;
    }
#endif
    const setup_code_sticker_t *st = sticker();
    if (st != nullptr) {
        *out = st->code;
        return true;
    }
#if OWN_CODE
    if (have) {
        *out = saved;
        return true;
    }
    static bool s_save_failed = false; // Try once a boot: the page asks every 2 s
    if (s_save_failed) {
        test_code(out);
        return false;
    }
    setup_code_t fresh;
    setup_code_generate(rand32, nullptr, &fresh);
    if (!save(&fresh)) {
        s_save_failed = true;
        test_code(out);
        return false;
    }
    ESP_LOGW(TAG, "Made a new setup code");
    *out = fresh;
    *created = true;
    return true;
#else
    test_code(out);
    return false;
#endif
}

// ---- Normal mode: the provider Matter pairs with ----

namespace {

class ShellyCommissionableData : public chip::DeviceLayer::CommissionableDataProvider
{
public:
    CHIP_ERROR GetSetupDiscriminator(uint16_t &discriminator) override
    {
        ReturnErrorOnFailure(Ensure());
        discriminator = m_code.discriminator;
        return CHIP_NO_ERROR;
    }
    CHIP_ERROR SetSetupDiscriminator(uint16_t) override { return CHIP_ERROR_NOT_IMPLEMENTED; }

    CHIP_ERROR GetSpake2pIterationCount(uint32_t &count) override
    {
        ReturnErrorOnFailure(Ensure());
        count = m_iterations;
        return CHIP_NO_ERROR;
    }

    CHIP_ERROR GetSpake2pSalt(chip::MutableByteSpan &salt) override
    {
        ReturnErrorOnFailure(Ensure());
        return chip::CopySpanToMutableSpan(chip::ByteSpan(m_salt, m_salt_len), salt);
    }

    CHIP_ERROR GetSpake2pVerifier(chip::MutableByteSpan &verifier, size_t &len) override
    {
        ReturnErrorOnFailure(Ensure());
        len = sizeof(m_verifier);
        return chip::CopySpanToMutableSpan(chip::ByteSpan(m_verifier), verifier);
    }

    CHIP_ERROR GetSetupPasscode(uint32_t &passcode) override
    {
        ReturnErrorOnFailure(Ensure());
        passcode = m_code.passcode;
        return CHIP_NO_ERROR;
    }
    CHIP_ERROR SetSetupPasscode(uint32_t) override { return CHIP_ERROR_NOT_IMPLEMENTED; }

    // Runs on the Matter thread (stack 8 KB), after Thread's radio is up,
    // the first time Matter asks: esp_random() is truly random by then. The
    // salt is new each boot; the verifier is computed once and kept.
    CHIP_ERROR Ensure()
    {
        pthread_mutex_lock(&s_lock);
        CHIP_ERROR err = m_ready ? CHIP_NO_ERROR : Prepare();
        pthread_mutex_unlock(&s_lock);
        return err;
    }

    void Log()
    {
        if (Ensure() != CHIP_NO_ERROR) {
            ESP_LOGE(TAG, "No setup code: pairing will fail");
            return;
        }
        char f[SETUP_CODE_FORMATTED_LEN];
        setup_code_manual_format(m_code.passcode, m_code.discriminator, f);
        const char *why;
        if (m_code.source == SETUP_CODE_SOURCE_STICKER) {
            why = "from the Shelly sticker";
        } else if (!OWN_CODE) {
            why = "the shared test code: no sticker code, and this build has no Wi-Fi mode";
        } else if (!m_saved) {
            why = "the shared test code: the device's own code could not be saved";
        } else if (m_code.source == SETUP_CODE_SOURCE_USER) {
            why = "yours";
        } else {
            why = m_created ? "made by this device, NEW on this boot" : "made by this device";
        }
        ESP_LOGW(TAG, "Matter setup code: %s (%s)", f, why);
    }

private:
    // With s_lock held.
    CHIP_ERROR Prepare()
    {
        m_saved = resolve(&m_code, &m_created);
        if (m_code.source == SETUP_CODE_SOURCE_STICKER && s_sticker.has_verifier) {
            // Serve the sticker's own salt, iterations and verifier, once the
            // verifier made from its passcode matches the stored one: that
            // proves the code was read right.
            ReturnErrorOnFailure(MakeVerifier(s_sticker.salt, s_sticker.salt_len, s_sticker.iterations));
            if (memcmp(m_verifier, s_sticker.verifier, sizeof(m_verifier)) != 0) {
                ESP_LOGE(TAG, "The sticker code doesn't match its stored verifier: not using it");
                s_sticker_state = -1;
                m_saved = resolve(&m_code, &m_created);
            } else {
                m_ready = true;
                return CHIP_NO_ERROR;
            }
        }
        // A new salt each boot; the verifier is computed once and kept.
        uint8_t salt[chip::Crypto::kSpake2p_Max_PBKDF_Salt_Length];
        esp_fill_random(salt, sizeof(salt));
        ReturnErrorOnFailure(MakeVerifier(salt, sizeof(salt), chip::Crypto::kSpake2p_Min_PBKDF_Iterations));
        m_ready = true;
        return CHIP_NO_ERROR;
    }

    CHIP_ERROR MakeVerifier(const uint8_t *salt, size_t salt_len, uint32_t iterations)
    {
        memcpy(m_salt, salt, salt_len);
        m_salt_len = salt_len;
        m_iterations = iterations;
        chip::Crypto::Spake2pVerifier v;
        ReturnErrorOnFailure(v.Generate(iterations, chip::ByteSpan(m_salt, m_salt_len), m_code.passcode));
        chip::MutableByteSpan out(m_verifier);
        return v.Serialize(out);
    }

    bool m_ready = false;
    bool m_saved = false;
    bool m_created = false;
    setup_code_t m_code = {};
    uint32_t m_iterations = chip::Crypto::kSpake2p_Min_PBKDF_Iterations;
    size_t m_salt_len = 0;
    uint8_t m_salt[chip::Crypto::kSpake2p_Max_PBKDF_Salt_Length] = {};
    uint8_t m_verifier[chip::Crypto::kSpake2p_VerifierSerialized_Length] = {};
};

ShellyCommissionableData s_provider;

} // namespace

void shelly_setup_code_install(void)
{
    esp_matter::set_custom_commissionable_data_provider(&s_provider);
}

static void log_work(intptr_t)
{
    s_provider.Log();
}

void shelly_setup_code_log(void)
{
    if (chip::DeviceLayer::PlatformMgr().ScheduleWork(log_work, 0) != CHIP_NO_ERROR) {
        ESP_LOGE(TAG, "Could not schedule the setup code log");
    }
}

// ---- Wi-Fi maintenance mode: show and change the code ----
// The change waits for 3 presses in shelly_settings.cpp; confirming it
// calls shelly_setup_code_save().

bool shelly_setup_code_current(setup_code_t *out, bool *created)
{
    pthread_mutex_lock(&s_lock);
    bool ok = resolve(out, created);
    pthread_mutex_unlock(&s_lock);
    return ok;
}

bool shelly_setup_code_sticker(setup_code_t *out)
{
    pthread_mutex_lock(&s_lock);
    const setup_code_sticker_t *st = sticker();
    if (st != nullptr) {
        *out = st->code;
    }
    pthread_mutex_unlock(&s_lock);
    return st != nullptr;
}

bool shelly_setup_code_qr(const setup_code_t *code, char out[SETUP_CODE_QR_MAX])
{
    if (code->source == SETUP_CODE_SOURCE_STICKER) {
        pthread_mutex_lock(&s_lock);
        const setup_code_sticker_t *st = sticker();
        bool same = st != nullptr && st->code.passcode == code->passcode &&
                    st->code.discriminator == code->discriminator;
        if (same) {
            strcpy(out, st->qr); // Exactly as printed on the sticker
        }
        pthread_mutex_unlock(&s_lock);
        if (same) {
            return true;
        }
    }
    setup_code_qr_t q = {};
    q.vendor_id = CONFIG_DEVICE_VENDOR_ID;
    q.product_id = CONFIG_DEVICE_PRODUCT_ID;
    q.capabilities = 0x02; // BLE
    q.discriminator = code->discriminator;
    q.passcode = code->passcode;
    return setup_code_qr_encode(&q, out);
}

setup_code_err_t shelly_setup_code_stage_manual(const char *text)
{
    uint32_t passcode;
    uint8_t short_disc;
    setup_code_err_t err = setup_code_manual_parse(text, &passcode, &short_disc);
    if (err != SETUP_CODE_OK) {
        return err;
    }
    shelly_change_t change = {};
    change.kind = SHELLY_CHANGE_SETUP_CODE;
    pthread_mutex_lock(&s_lock);
    const setup_code_sticker_t *st = sticker();
    if (setup_code_is_sticker(st, passcode, short_disc)) {
        change.code = st->code; // The sticker's own: its full discriminator, so its QR works too
    } else {
        change.code.passcode = passcode;
        change.code.discriminator = setup_code_discriminator_from_short(short_disc, (uint8_t)esp_random());
        change.code.source = SETUP_CODE_SOURCE_USER;
    }
    pthread_mutex_unlock(&s_lock);
    shelly_change_stage(&change);
    return SETUP_CODE_OK;
}

void shelly_setup_code_stage_random(void)
{
    shelly_change_t change = {};
    change.kind = SHELLY_CHANGE_SETUP_CODE;
    setup_code_generate(rand32, nullptr, &change.code);
    shelly_change_stage(&change);
}

bool shelly_setup_code_save(const setup_code_t *code)
{
    if (!OWN_CODE) {
        return false;
    }
    pthread_mutex_lock(&s_lock);
    bool ok = save(code);
    pthread_mutex_unlock(&s_lock);
    if (ok) {
        ESP_LOGW(TAG, "%s; it applies from the next normal boot",
                 code->source == SETUP_CODE_SOURCE_STICKER ? "Back to the sticker code" : "New setup code saved");
    }
    return ok;
}

void shelly_setup_code_factory_reset(void)
{
#if OWN_CODE
    pthread_mutex_lock(&s_lock);
    setup_code_t saved;
    bool user = load(&saved) && saved.source == SETUP_CODE_SOURCE_USER;
    if (user && forget_user_code()) {
        ESP_LOGW(TAG, "Factory reset: your own setup code is forgotten");
    }
    pthread_mutex_unlock(&s_lock);
#endif
}
