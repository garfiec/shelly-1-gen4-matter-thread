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

// protection.cpp

#include <sdkconfig.h>

#ifdef CONFIG_SHELLY_HAS_BL0942

#include "protection.h"
#include "protection_logic.h"

#include <stddef.h>

#include <esp_attr.h>
#include <esp_log.h>
#include <esp_rom_crc.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <nvs.h>

#include <esp_matter.h>

#include <relay.h>
#include <shelly_app.h>
#include <shelly_faults.h>

using namespace chip::app::Clusters;
using namespace esp_matter;

static const char *TAG = "protection";

// Limits (Kconfig, UL-rated defaults). See docs/PROTECTION.md.
#define RATED_A             (CONFIG_SHELLY_PROT_RATED_CURRENT_MA / 1000.0f)
#define PICKUP_A            (RATED_A * CONFIG_SHELLY_PROT_PICKUP_PERCENT / 100.0f)
#define I2T_K_A2S           ((float)CONFIG_SHELLY_PROT_I2T_K_A2S)
#define I2T_TAU_S           ((float)CONFIG_SHELLY_PROT_I2T_TAU_S)
#define CURRENT_FULL_SCALE_A (CONFIG_SHELLY_BL0942_CURRENT_FULL_SCALE_MA / 1000.0f)
#define OV_SUSTAIN_MS       (CONFIG_SHELLY_PROT_OV_SUSTAIN_S * 1000LL)
#define OV_RECOVER_MS       (CONFIG_SHELLY_PROT_OV_RECOVER_S * 1000LL)
#define COOLDOWN_US         (CONFIG_SHELLY_PROT_COOLDOWN_MS * 1000LL)
#define METER_LOST_MS       (CONFIG_SHELLY_PROT_METER_LOST_S * 1000LL)

// Mains present for nominal classification: 80 V for 3 RMS updates.
#define NOMINAL_PRESENT_V   80.0f
#define NOMINAL_SETTLE      3
#define NOMINAL_SPLIT_V     180.0f
#define NOMINAL_FEED_MS     1000

// Welded relay: commanded off 2s, 0.5 A for 5s alarms; below 0.2 A for 10s clears.
#define WELD_ALARM_A        0.5f
#define WELD_CLEAR_A        0.2f
#define WELD_SETTLE_MS      2000
#define WELD_ALARM_MS       5000
#define WELD_CLEAR_MS       10000

// A delayed poll integrates its real elapsed time, up to this cap.
#define MAX_DT_S            5.0f

// The learned nominal lives in esp-matter's own NVS namespace, which
// esp_matter::factory_reset() erases: a factory reset relearns it, so a
// device moved to another installation classifies its new supply.
#define NOMINAL_NVS_NAMESPACE   "esp_matter_kvs"
#define NOMINAL_NVS_KEY         "shelly_nominal"

// ---------------------------------------------------------------------------
// Overcurrent latch in RTC memory
// ---------------------------------------------------------------------------
//
// RTC_NOINIT memory survives a software reset (panic, watchdog, OTA reboot)
// but holds garbage after power-on. A magic value and a CRC reject garbage,
// and a power-on reset clears the latch outright: a power cycle is one of
// the ways to reset a trip.

#define LATCH_MAGIC 0x53484C54u  // "SHLT"

typedef struct {
    uint32_t magic;
    uint32_t latched;
    uint32_t crc;
} rtc_latch_t;

static RTC_NOINIT_ATTR rtc_latch_t s_rtc_latch;

static uint32_t rtc_latch_crc(const rtc_latch_t *l)
{
    return esp_rom_crc32_le(0, (const uint8_t *)l, offsetof(rtc_latch_t, crc));
}

static void rtc_latch_store(bool latched)
{
    s_rtc_latch.magic = LATCH_MAGIC;
    s_rtc_latch.latched = latched ? 1 : 0;
    s_rtc_latch.crc = rtc_latch_crc(&s_rtc_latch);
}

static bool rtc_latch_load(void)
{
    if (esp_reset_reason() == ESP_RST_POWERON) {
        rtc_latch_store(false);
        return false;
    }
    if (s_rtc_latch.magic != LATCH_MAGIC || s_rtc_latch.crc != rtc_latch_crc(&s_rtc_latch)) {
        rtc_latch_store(false);
        return false;
    }
    return s_rtc_latch.latched != 0;
}

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

static uint16_t s_endpoint_id = 0;

// Read by the relay gate from any task.
static volatile bool s_latched = false;
static volatile bool s_ov_active = false;
// 64-bit, so it is read and written under a lock: a plain load on this
// 32-bit core can tear against a trip on another task.
static portMUX_TYPE s_cooldown_mux = portMUX_INITIALIZER_UNLOCKED;
static int64_t s_cooldown_until_us = 0;

// Overvoltage restore. Set on the metering task, cancelled from the Matter
// thread or an input task.
static volatile bool s_restore_pending = false;
static bool s_restore_on = false;

// Only the metering task touches these.
static i2t_t s_i2t;
static nominal_classifier_t s_classifier;
static mains_nominal_t s_nominal = MAINS_NOMINAL_UNKNOWN;
static overvoltage_t s_ov;
static welded_t s_welded;
static int64_t s_last_us = -1;
static int64_t s_last_valid_ms = 0;
static int64_t s_last_classify_ms = -NOMINAL_FEED_MS;
static bool s_overcurrent_logged = false;

static void cooldown_start(int64_t now_us)
{
    portENTER_CRITICAL(&s_cooldown_mux);
    s_cooldown_until_us = now_us + COOLDOWN_US;
    portEXIT_CRITICAL(&s_cooldown_mux);
}

static bool cooldown_active(void)
{
    int64_t now_us = esp_timer_get_time();
    portENTER_CRITICAL(&s_cooldown_mux);
    bool cooling = now_us < s_cooldown_until_us;
    portEXIT_CRITICAL(&s_cooldown_mux);
    return cooling;
}

static const char *protection_gate(void)
{
    if (s_latched) {
        return "overcurrent trip latched";
    }
    if (s_ov_active) {
        return "overvoltage";
    }
    if (cooldown_active()) {
        return "post-trip cooldown";
    }
    return nullptr;
}

static void set_latched(bool latched)
{
    s_latched = latched;
    rtc_latch_store(latched);
    shelly_fault_set(SHELLY_FAULT_SRC_OVERCURRENT, latched);
}

// A deliberate On from the user clears a latched overcurrent trip, but
// only one the relay gate can accept: during overvoltage or the 2 s
// cooldown the gate refuses the On anyway, and clearing the latch for it
// would leave a tripped load that the next restart with StartUpOnOff On
// re-energizes. (A local toggle that another gate refuses, such as
// over-temperature, is re-latched by local_toggle_done().)
static void user_on(void)
{
    if (!s_latched) {
        return;
    }
    if (s_ov_active || cooldown_active()) {
        ESP_LOGW(TAG, "On refused by the gate: the overcurrent latch stays");
        return;
    }
    ESP_LOGW(TAG, "Deliberate On: clearing the overcurrent latch");
    set_latched(false);
}

// Any deliberate switching during an overvoltage trip overrides the
// automatic restore.
static void user_switched(void)
{
    if (s_restore_pending) {
        ESP_LOGI(TAG, "User switched during overvoltage trip: restore cancelled");
        s_restore_pending = false;
    }
}

static bool onoff_read(uint32_t attribute_id, esp_matter_attr_val_t *val)
{
    attribute_t *attribute = attribute::get(s_endpoint_id, OnOff::Id, attribute_id);
    return attribute != nullptr && attribute::get_val(attribute, val) == ESP_OK;
}

static bool onoff_is_on(void)
{
    esp_matter_attr_val_t val = esp_matter_invalid(nullptr);
    return onoff_read(OnOff::Attributes::OnOff::Id, &val) ? val.val.b : relay_get();
}

// The OnOff server discards OnWithRecallGlobalScene while GlobalSceneControl
// is TRUE, which any earlier On leaves it. Unknown counts as TRUE.
static bool global_scene_control(void)
{
    esp_matter_attr_val_t val = esp_matter_invalid(nullptr);
    return !onoff_read(OnOff::Attributes::GlobalSceneControl::Id, &val) || val.val.b;
}

// While off with OffWaitTime above 0 (the delayed-off guard after a timed
// On), the server answers OnWithTimedOff by shortening that guard only.
static bool off_wait_pending(void)
{
    esp_matter_attr_val_t val = esp_matter_invalid(nullptr);
    return onoff_read(OnOff::Attributes::OffWaitTime::Id, &val) && val.val.u16 > 0;
}

// Runs on the Matter thread BEFORE the standard OnOff command handler.
// The standard handler only runs if this returns ESP_OK, so it must
// ALWAYS return ESP_OK: it observes commands, it never refuses them. The
// relay gate does the refusing, in the attribute write that follows.
static esp_err_t onoff_command_cb(const ConcreteCommandPath &command_path, TLVReader &tlv_data, void *opaque_ptr)
{
    if (command_path.mEndpointId != s_endpoint_id || command_path.mClusterId != OnOff::Id) {
        return ESP_OK;
    }
    // Only a command the OnOff server will act on as an On is a deliberate
    // On. The server discards the ones below without turning anything on,
    // and clearing the latch for them would leave a tripped load that the
    // next restart with StartUpOnOff On re-energizes.
    switch (command_path.mCommandId) {
    case OnOff::Commands::OnWithTimedOff::Id: {
        // Not an On: a command that does not decode (the server drops it),
        // AcceptOnlyWhenOn while off (it only extends an on-timer), or any
        // while off in the delayed-off guard (it only shortens the guard).
        OnOff::Commands::OnWithTimedOff::DecodableType command;
        if (command.Decode(tlv_data) != CHIP_NO_ERROR) {
            break;
        }
        bool on = onoff_is_on();
        if (!on && (command.onOffControl.Has(OnOff::OnOffControlBitmap::kAcceptOnlyWhenOn) || off_wait_pending())) {
            break;
        }
        user_on();
        break;
    }
    case OnOff::Commands::OnWithRecallGlobalScene::Id:
        if (!global_scene_control()) {
            user_on();
        }
        break;
    case OnOff::Commands::On::Id:
        user_on();
        break;
    case OnOff::Commands::Toggle::Id:
        user_switched();
        if (!onoff_is_on()) {
            user_on();
        }
        break;
    case OnOff::Commands::Off::Id:
    case OnOff::Commands::OffWithEffect::Id:
        user_switched();
        break;
    default:
        break;
    }
    return ESP_OK;
}

// The local-toggle hooks: shelly_onoff_toggle() runs them around every
// button press and wall-switch flip, under the CHIP stack lock. A toggle
// from off clears the latch first, so the relay gate can let it through.
// If the relay refuses it anyway (the 2 s cooldown, over-temperature,
// overvoltage), the latch comes back: only a tap or flip that turns the
// outlet on clears it (docs/PROTECTION.md).
static bool s_unlatched_by_toggle = false;

static void local_toggle_hook(bool currently_on)
{
    user_switched();
    bool was_latched = s_latched;
    if (!currently_on) {
        user_on();
    }
    s_unlatched_by_toggle = was_latched && !s_latched;
}

static void local_toggle_done(bool accepted)
{
    if (s_unlatched_by_toggle && !accepted) {
        ESP_LOGW(TAG, "Turn-on refused: the overcurrent latch stays");
        set_latched(true);
    }
    s_unlatched_by_toggle = false;
}

bool protection_is_latched(void)
{
    return s_latched;
}

esp_err_t protection_attach_endpoint(uint16_t relay_endpoint_id)
{
    s_endpoint_id = relay_endpoint_id;

    static const uint32_t commands[] = {
        OnOff::Commands::Off::Id,
        OnOff::Commands::On::Id,
        OnOff::Commands::Toggle::Id,
        OnOff::Commands::OffWithEffect::Id,
        OnOff::Commands::OnWithRecallGlobalScene::Id,
        OnOff::Commands::OnWithTimedOff::Id,
    };
    for (uint32_t id : commands) {
        command_t *command = command::get(relay_endpoint_id, OnOff::Id, id);
        if (command != nullptr) {
            command::set_user_callback(command, onoff_command_cb);
        }
    }
    protection_attach_local_inputs();
    return ESP_OK;
}

void protection_attach_local_inputs(void)
{
    shelly_onoff_set_local_toggle_hook(local_toggle_hook, local_toggle_done);
}

// ---------------------------------------------------------------------------
// Trips
// ---------------------------------------------------------------------------

// Every trip, in this order: the gate state that refuses turn-on, then the
// relay opens under the relay lock, then the bookkeeping (RTC latch, fault
// registry, log), then Matter is told asynchronously. Nothing on the way to
// opening the relay logs, writes memory it does not need, or waits on the
// CHIP lock.
static void trip_overcurrent(float amps, int64_t now_us)
{
    s_latched = true;
    cooldown_start(now_us);
    relay_trip();

    rtc_latch_store(true);
    shelly_fault_set(SHELLY_FAULT_SRC_OVERCURRENT, true);
    ESP_LOGE(TAG, "OVERCURRENT TRIP at %.1f A (heat %.0f A²s): relay latched off", amps, s_i2t.heat_a2s);
    shelly_onoff_set_async(s_endpoint_id, false);
}

static void trip_overvoltage(float volts, int64_t now_us)
{
    bool was_on = relay_get();
    s_ov_active = true;
    cooldown_start(now_us);
    relay_trip();

    s_restore_on = was_on;
    s_restore_pending = true;
    shelly_fault_set(SHELLY_FAULT_SRC_OVERVOLTAGE, true);
    ESP_LOGE(TAG, "OVERVOLTAGE TRIP at %.0f V (limit %.0f V): relay off, was %s", volts, s_ov.limit_v,
             was_on ? "on" : "off");
    shelly_onoff_set_async(s_endpoint_id, false);
}

static void recover_overvoltage(void)
{
    s_ov_active = false;
    shelly_fault_set(SHELLY_FAULT_SRC_OVERVOLTAGE, false);
    bool restore = s_restore_pending && s_restore_on;
    s_restore_pending = false;
    ESP_LOGW(TAG, "Voltage back in range for %ds%s", CONFIG_SHELLY_PROT_OV_RECOVER_S,
             restore ? ": restoring relay on" : "");
    if (restore) {
        // Goes through the attribute write and the full relay gate: a
        // latched overcurrent, over-temperature or cooldown still refuses it.
        shelly_onoff_set_async(s_endpoint_id, true);
    }
}

// ---------------------------------------------------------------------------
// Nominal voltage
// ---------------------------------------------------------------------------

static void nominal_load(void)
{
    nvs_handle_t h;
    uint16_t v = 0;
    if (nvs_open(NOMINAL_NVS_NAMESPACE, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u16(h, NOMINAL_NVS_KEY, &v);
        nvs_close(h);
    }
    if (v == MAINS_NOMINAL_120V || v == MAINS_NOMINAL_240V) {
        s_nominal = (mains_nominal_t)v;
    }
}

static void nominal_store(mains_nominal_t nominal)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NOMINAL_NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = nvs_set_u16(h, NOMINAL_NVS_KEY, (uint16_t)nominal);
        if (err == ESP_OK) {
            err = nvs_commit(h);
        }
        nvs_close(h);
    }
    if (err != ESP_OK) {
        // Protection still runs this boot; the nominal is relearned next boot.
        ESP_LOGW(TAG, "Failed to store mains nominal: %s", esp_err_to_name(err));
    }
}

static void nominal_apply(void)
{
    float limit = overvoltage_limit_for(s_nominal);
    overvoltage_init(&s_ov, limit, OV_SUSTAIN_MS, OV_RECOVER_MS);
    if (s_nominal == MAINS_NOMINAL_UNKNOWN) {
        ESP_LOGW(TAG, "Mains nominal not learned yet: no overvoltage protection until it is");
    } else {
        ESP_LOGI(TAG, "Mains nominal %d V, overvoltage limit %.0f V", (int)s_nominal, limit);
    }
}

// ---------------------------------------------------------------------------
// Feed
// ---------------------------------------------------------------------------

void protection_feed(const protection_sample_t *sample)
{
    int64_t now_us = sample->now_us;
    int64_t now_ms = now_us / 1000;

    if (!sample->valid) {
        if (now_ms - s_last_valid_ms >= METER_LOST_MS) {
            shelly_fault_set(SHELLY_FAULT_SRC_METER_LOST, true);
        }
        return;
    }
    s_last_valid_ms = now_ms;
    shelly_fault_set(SHELLY_FAULT_SRC_METER_LOST, false);

    // dt runs from the previous valid sample, so polls lost in between
    // still count at this sample's current.
    float dt_s = s_last_us < 0 ? 0.0f : (float)(now_us - s_last_us) / 1e6f;
    if (dt_s > MAX_DT_S) {
        dt_s = MAX_DT_S;
    }
    s_last_us = now_us;

    // A clipped current channel reads low; count it as at least full scale.
    float amps = sample->amps;
    if (sample->amps_clipped && amps < CURRENT_FULL_SCALE_A) {
        amps = CURRENT_FULL_SCALE_A;
    }

    // Overcurrent. Heat builds only while the relay is closed: once a trip
    // opens it, the meter's RMS registers still read the pre-trip current
    // for up to one 800 ms refresh, and counting that would push the heat
    // past the trip level for up to a minute, so an On long after the 2 s
    // cooldown would re-trip at the next poll whatever the load draws. For
    // the same reason the heat is capped at the trip level when it trips.
    bool over = i2t_update(&s_i2t, relay_get() ? amps : 0.0f, dt_s);
    if (over && !s_latched) {
        trip_overcurrent(amps, now_us);
        s_i2t.heat_a2s = I2T_K_A2S;
    }
    if (amps > PICKUP_A && !s_overcurrent_logged) {
        ESP_LOGW(TAG, "Current %.1f A above %.1f A pickup: trip in %.0fs at this level", amps, PICKUP_A,
                 i2t_trip_time_s(&s_i2t, amps));
        s_overcurrent_logged = true;
    } else if (amps <= PICKUP_A) {
        s_overcurrent_logged = false;
    }

    // Learn the nominal once, from settled readings.
    if (s_nominal == MAINS_NOMINAL_UNKNOWN && now_ms - s_last_classify_ms >= NOMINAL_FEED_MS) {
        s_last_classify_ms = now_ms;
        mains_nominal_t n = nominal_classifier_update(&s_classifier, sample->volts);
        if (n != MAINS_NOMINAL_UNKNOWN) {
            s_nominal = n;
            nominal_store(n);
            nominal_apply();
        }
    }

    // Overvoltage.
    switch (overvoltage_update(&s_ov, sample->volts, now_ms)) {
    case OV_EVENT_TRIP:
        trip_overvoltage(sample->volts, now_us);
        break;
    case OV_EVENT_RECOVER:
        recover_overvoltage();
        break;
    default:
        break;
    }

    // Welded relay (alert only).
    if (welded_update(&s_welded, relay_get(), amps, now_ms)) {
        if (s_welded.alarmed) {
            ESP_LOGE(TAG, "RELAY WELDED? %.2f A flowing while commanded off", amps);
        } else {
            ESP_LOGI(TAG, "Welded-relay alert cleared");
        }
        shelly_fault_set(SHELLY_FAULT_SRC_WELDED_RELAY, s_welded.alarmed);
    }
}

esp_err_t protection_init(void)
{
    i2t_init(&s_i2t, PICKUP_A, I2T_K_A2S, I2T_TAU_S);
    nominal_classifier_init(&s_classifier, NOMINAL_PRESENT_V, NOMINAL_SETTLE, NOMINAL_SPLIT_V);
    welded_init(&s_welded, WELD_ALARM_A, WELD_CLEAR_A, WELD_SETTLE_MS, WELD_ALARM_MS, WELD_CLEAR_MS);
    s_last_valid_ms = esp_timer_get_time() / 1000;

    nominal_load();
    nominal_apply();

    if (rtc_latch_load()) {
        s_latched = true;
        shelly_fault_set(SHELLY_FAULT_SRC_OVERCURRENT, true);
        ESP_LOGW(TAG, "Overcurrent trip still latched after a software reset: relay stays off");
    }

    ESP_LOGI(TAG, "Overcurrent: pickup %.1f A, k %.0f A²s (trip %.0fs at %.1f A, %.1fs at %.1f A)",
             PICKUP_A, I2T_K_A2S, i2t_trip_time_s(&s_i2t, RATED_A * 1.2f), RATED_A * 1.2f,
             i2t_trip_time_s(&s_i2t, CURRENT_FULL_SCALE_A), CURRENT_FULL_SCALE_A);
    return relay_register_gate(protection_gate);
}

#endif // CONFIG_SHELLY_HAS_BL0942
