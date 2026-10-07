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
#include <string.h>

#include <esp_log.h>
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
#define OWN_CODE 0 // No Wi-Fi page to show or set a code: always the test code
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

static bool save(const setup_code_t *code)
{
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

#if OWN_CODE
// The saved code, or a new one saved now. Call with s_lock held, and only
// once a radio is on: esp_random() is only truly random then.
static bool load_or_create(setup_code_t *out, bool *created)
{
    static bool s_save_failed = false; // Try once a boot: the page asks every 2 s
    *created = false;
    if (load(out)) {
        return true;
    }
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
}
#endif

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
        count = chip::Crypto::kSpake2p_Min_PBKDF_Iterations;
        return CHIP_NO_ERROR;
    }

    CHIP_ERROR GetSpake2pSalt(chip::MutableByteSpan &salt) override
    {
        ReturnErrorOnFailure(Ensure());
        return chip::CopySpanToMutableSpan(chip::ByteSpan(m_salt), salt);
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
        if (!OWN_CODE) {
            why = "the shared test code: this build has no Wi-Fi mode";
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
#if OWN_CODE
        m_saved = load_or_create(&m_code, &m_created);
#else
        test_code(&m_code);
#endif
        esp_fill_random(m_salt, sizeof(m_salt));
        chip::Crypto::Spake2pVerifier v;
        ReturnErrorOnFailure(v.Generate(chip::Crypto::kSpake2p_Min_PBKDF_Iterations, chip::ByteSpan(m_salt),
                                        m_code.passcode));
        chip::MutableByteSpan out(m_verifier);
        ReturnErrorOnFailure(v.Serialize(out));
        m_ready = true;
        return CHIP_NO_ERROR;
    }

    bool m_ready = false;
    bool m_saved = false;
    bool m_created = false;
    setup_code_t m_code = {};
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
#if OWN_CODE
    pthread_mutex_lock(&s_lock);
    bool ok = load_or_create(out, created);
    pthread_mutex_unlock(&s_lock);
    return ok;
#else
    test_code(out);
    *created = false;
    return false;
#endif
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
    change.code.passcode = passcode;
    change.code.discriminator = setup_code_discriminator_from_short(short_disc, (uint8_t)esp_random());
    change.code.source = SETUP_CODE_SOURCE_USER;
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
        ESP_LOGW(TAG, "New setup code saved; it applies from the next normal boot");
    }
    return ok;
}
