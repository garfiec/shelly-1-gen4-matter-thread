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

#ifdef CONFIG_SHELLY_HAS_METER

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

static_assert(RELAY_CHANNELS <= PROTECTION_MAX_CHANNELS, "protection_sample_t holds too few channels");
#define CHANNELS RELAY_CHANNELS

// Limits (Kconfig, UL-rated defaults). See docs/PROTECTION.md.
#define RATED_A             (CONFIG_SHELLY_PROT_RATED_CURRENT_MA / 1000.0f)
#define PICKUP_A            (RATED_A * CONFIG_SHELLY_PROT_PICKUP_PERCENT / 100.0f)
#define I2T_K_A2S           ((float)CONFIG_SHELLY_PROT_I2T_K_A2S)
#define I2T_TAU_S           ((float)CONFIG_SHELLY_PROT_I2T_TAU_S)
#define OV_SUSTAIN_MS       (CONFIG_SHELLY_PROT_OV_SUSTAIN_S * 1000LL)
#define OV_RECOVER_MS       (CONFIG_SHELLY_PROT_OV_RECOVER_S * 1000LL)
#define COOLDOWN_US         (CONFIG_SHELLY_PROT_COOLDOWN_MS * 1000LL)
#define METER_LOST_MS       (CONFIG_SHELLY_PROT_METER_LOST_S * 1000LL)

// The whole-device model: the same pickup margin and the same trip times
// as a channel, scaled to the total rating (k grows with the square).
#define TOTAL_RATED_A       (CONFIG_SHELLY_PROT_TOTAL_RATED_CURRENT_MA / 1000.0f)
#define HAS_TOTAL           (CHANNELS > 1 && CONFIG_SHELLY_PROT_TOTAL_RATED_CURRENT_MA > 0)
#define TOTAL_PICKUP_A      (TOTAL_RATED_A * CONFIG_SHELLY_PROT_PICKUP_PERCENT / 100.0f)
#define TOTAL_K_A2S         (I2T_K_A2S * (TOTAL_RATED_A / RATED_A) * (TOTAL_RATED_A / RATED_A))

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

#define ALL_CHANNELS        ((1u << CHANNELS) - 1u)

// ---------------------------------------------------------------------------
// Overcurrent latches in RTC memory
// ---------------------------------------------------------------------------
//
// RTC_NOINIT memory survives a software reset (panic, watchdog, OTA reboot)
// but holds garbage after power-on. A magic value and a CRC reject garbage,
// and a power-on reset clears the latches outright: a power cycle is one of
// the ways to reset a trip. Bit n is relay n+1 (the single-relay layout
// stored 1 for a trip, which reads the same).

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

static void rtc_latch_store(uint32_t latched)
{
    s_rtc_latch.magic = LATCH_MAGIC;
    s_rtc_latch.latched = latched & ALL_CHANNELS;
    s_rtc_latch.crc = rtc_latch_crc(&s_rtc_latch);
}

static uint32_t rtc_latch_load(void)
{
    if (esp_reset_reason() == ESP_RST_POWERON) {
        rtc_latch_store(0);
        return 0;
    }
    if (s_rtc_latch.magic != LATCH_MAGIC || s_rtc_latch.crc != rtc_latch_crc(&s_rtc_latch)) {
        rtc_latch_store(0);
        return 0;
    }
    return s_rtc_latch.latched & ALL_CHANNELS;
}

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

static uint16_t s_endpoint_id[CHANNELS];

// Read by the relay gate from any task. Bit n: relay n+1 latched off.
static volatile uint32_t s_latched = 0;
static volatile bool s_ov_active = false;
// 64-bit, so they are read and written under a lock: a plain load on this
// 32-bit core can tear against a trip on another task.
static portMUX_TYPE s_cooldown_mux = portMUX_INITIALIZER_UNLOCKED;
static int64_t s_cooldown_until_us[CHANNELS];

// Overvoltage restore, bit n for relay n+1. Set on the metering task,
// cancelled from the Matter thread or an input task.
static portMUX_TYPE s_restore_mux = portMUX_INITIALIZER_UNLOCKED;
static uint32_t s_restore_pending = 0;
static uint32_t s_restore_on = 0;

// Only the metering task touches these.
static i2t_t s_i2t[CHANNELS];
#if HAS_TOTAL
static i2t_t s_i2t_total;
static bool s_total_logged = false;
#endif
static nominal_classifier_t s_classifier;
static mains_nominal_t s_nominal = MAINS_NOMINAL_UNKNOWN;
static overvoltage_t s_ov;
static welded_t s_welded[CHANNELS];
static int64_t s_last_us = -1;
static int64_t s_last_valid_ms = 0;
static int64_t s_last_classify_ms = -NOMINAL_FEED_MS;
static bool s_overcurrent_logged[CHANNELS];

static void cooldown_start(uint32_t channels, int64_t now_us)
{
    portENTER_CRITICAL(&s_cooldown_mux);
    for (int ch = 0; ch < CHANNELS; ch++) {
        if (channels & (1u << ch)) {
            s_cooldown_until_us[ch] = now_us + COOLDOWN_US;
        }
    }
    portEXIT_CRITICAL(&s_cooldown_mux);
}

static bool cooldown_active(int ch)
{
    int64_t now_us = esp_timer_get_time();
    portENTER_CRITICAL(&s_cooldown_mux);
    bool cooling = now_us < s_cooldown_until_us[ch];
    portEXIT_CRITICAL(&s_cooldown_mux);
    return cooling;
}

static const char *protection_gate(int ch)
{
    if (ch < 0 || ch >= CHANNELS) {
        return nullptr;
    }
    if (s_latched & (1u << ch)) {
        return "overcurrent trip latched";
    }
    if (s_ov_active) {
        return "overvoltage";
    }
    if (cooldown_active(ch)) {
        return "post-trip cooldown";
    }
    return nullptr;
}

// The fault is shown while any relay is latched.
static void latch_publish(void)
{
    rtc_latch_store(s_latched);
    shelly_fault_set(SHELLY_FAULT_SRC_OVERCURRENT, s_latched != 0);
}

static void set_latched(int ch, bool latched)
{
    if (latched) {
        s_latched |= 1u << ch;
    } else {
        s_latched &= ~(1u << ch);
    }
    latch_publish();
}

static int endpoint_channel(uint16_t endpoint_id)
{
    for (int ch = 0; ch < CHANNELS; ch++) {
        if (endpoint_id != 0 && s_endpoint_id[ch] == endpoint_id) {
            return ch;
        }
    }
    return -1;
}

// A deliberate On from the user clears that relay's latched overcurrent
// trip, but only one the relay gate can accept: during overvoltage or the
// 2 s cooldown the gate refuses the On anyway, and clearing the latch for
// it would leave a tripped load that the next restart with StartUpOnOff On
// re-energizes. (A local toggle that another gate refuses, such as
// over-temperature, is re-latched by local_toggle_done().)
static void user_on(int ch)
{
    if (!(s_latched & (1u << ch))) {
        return;
    }
    if (s_ov_active || cooldown_active(ch)) {
        ESP_LOGW(TAG, "On refused by the gate: the overcurrent latch stays");
        return;
    }
    ESP_LOGW(TAG, "Deliberate On: clearing the overcurrent latch on relay %d", ch + 1);
    set_latched(ch, false);
}

// Any deliberate switching of a relay during an overvoltage trip overrides
// its automatic restore.
static void user_switched(int ch)
{
    portENTER_CRITICAL(&s_restore_mux);
    bool was = (s_restore_pending & (1u << ch)) != 0;
    s_restore_pending &= ~(1u << ch);
    portEXIT_CRITICAL(&s_restore_mux);
    if (was) {
        ESP_LOGI(TAG, "User switched relay %d during overvoltage trip: its restore is cancelled", ch + 1);
    }
}

static bool onoff_read(int ch, uint32_t attribute_id, esp_matter_attr_val_t *val)
{
    attribute_t *attribute = attribute::get(s_endpoint_id[ch], OnOff::Id, attribute_id);
    return attribute != nullptr && attribute::get_val(attribute, val) == ESP_OK;
}

static bool onoff_is_on(int ch)
{
    esp_matter_attr_val_t val = esp_matter_invalid(nullptr);
    return onoff_read(ch, OnOff::Attributes::OnOff::Id, &val) ? val.val.b : relay_get_ch(ch);
}

// The OnOff server discards OnWithRecallGlobalScene while GlobalSceneControl
// is TRUE, which any earlier On leaves it. Unknown counts as TRUE.
static bool global_scene_control(int ch)
{
    esp_matter_attr_val_t val = esp_matter_invalid(nullptr);
    return !onoff_read(ch, OnOff::Attributes::GlobalSceneControl::Id, &val) || val.val.b;
}

// While off with OffWaitTime above 0 (the delayed-off guard after a timed
// On), the server answers OnWithTimedOff by shortening that guard only.
static bool off_wait_pending(int ch)
{
    esp_matter_attr_val_t val = esp_matter_invalid(nullptr);
    return onoff_read(ch, OnOff::Attributes::OffWaitTime::Id, &val) && val.val.u16 > 0;
}

// Runs on the Matter thread BEFORE the standard OnOff command handler.
// The standard handler only runs if this returns ESP_OK, so it must
// ALWAYS return ESP_OK: it observes commands, it never refuses them. The
// relay gate does the refusing, in the attribute write that follows.
static esp_err_t onoff_command_cb(const ConcreteCommandPath &command_path, TLVReader &tlv_data, void *opaque_ptr)
{
    int ch = endpoint_channel(command_path.mEndpointId);
    if (ch < 0 || command_path.mClusterId != OnOff::Id) {
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
        bool on = onoff_is_on(ch);
        if (!on && (command.onOffControl.Has(OnOff::OnOffControlBitmap::kAcceptOnlyWhenOn) || off_wait_pending(ch))) {
            break;
        }
        user_on(ch);
        break;
    }
    case OnOff::Commands::OnWithRecallGlobalScene::Id:
        if (!global_scene_control(ch)) {
            user_on(ch);
        }
        break;
    case OnOff::Commands::On::Id:
        user_on(ch);
        break;
    case OnOff::Commands::Toggle::Id:
        user_switched(ch);
        if (!onoff_is_on(ch)) {
            user_on(ch);
        }
        break;
    case OnOff::Commands::Off::Id:
    case OnOff::Commands::OffWithEffect::Id:
        user_switched(ch);
        break;
    default:
        break;
    }
    return ESP_OK;
}

// The local-toggle hooks: shelly_onoff_toggle() and shelly_onoff_toggle_to()
// run them around every button press and wall-switch change, under the CHIP
// stack lock, with the relay and the state the user asked for. A turn-on
// clears that relay's latch first, so the relay gate can let it through.
// If the relay refuses it anyway (the 2 s cooldown, over-temperature,
// overvoltage), the latch comes back: only a tap or flip that turns the
// relay on clears it (docs/PROTECTION.md).
static uint32_t s_unlatched_by_toggle = 0;

static void local_toggle_hook(int ch, bool turning_on)
{
    if (ch < 0 || ch >= CHANNELS) {
        return;
    }
    user_switched(ch);
    bool was_latched = (s_latched & (1u << ch)) != 0;
    if (turning_on) {
        user_on(ch);
    }
    if (was_latched && !(s_latched & (1u << ch))) {
        s_unlatched_by_toggle |= 1u << ch;
    } else {
        s_unlatched_by_toggle &= ~(1u << ch);
    }
}

static void local_toggle_done(int ch, bool accepted)
{
    if (ch < 0 || ch >= CHANNELS) {
        return;
    }
    if ((s_unlatched_by_toggle & (1u << ch)) && !accepted) {
        ESP_LOGW(TAG, "Turn-on refused: the overcurrent latch stays");
        set_latched(ch, true);
    }
    s_unlatched_by_toggle &= ~(1u << ch);
}

bool protection_is_latched_ch(int ch)
{
    return ch >= 0 && ch < CHANNELS && (s_latched & (1u << ch)) != 0;
}

bool protection_is_latched(void)
{
    return protection_is_latched_ch(0);
}

esp_err_t protection_attach_endpoint_ch(int ch, uint16_t relay_endpoint_id)
{
    if (ch < 0 || ch >= CHANNELS) {
        return ESP_ERR_INVALID_ARG;
    }
    s_endpoint_id[ch] = relay_endpoint_id;

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

esp_err_t protection_attach_endpoint(uint16_t relay_endpoint_id)
{
    return protection_attach_endpoint_ch(0, relay_endpoint_id);
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
static void trip_overcurrent(int ch, float amps, int64_t now_us)
{
    s_latched |= 1u << ch;
    cooldown_start(1u << ch, now_us);
    relay_trip_ch(ch);

    latch_publish();
#if CHANNELS > 1
    ESP_LOGE(TAG, "OVERCURRENT TRIP on relay %d at %.1f A (heat %.0f A²s): relay latched off", ch + 1, amps,
             s_i2t[ch].heat_a2s);
#else
    ESP_LOGE(TAG, "OVERCURRENT TRIP at %.1f A (heat %.0f A²s): relay latched off", amps, s_i2t[ch].heat_a2s);
#endif
    shelly_onoff_set_async(s_endpoint_id[ch], false);
}

#if HAS_TOTAL
static void trip_total(float amps, int64_t now_us)
{
    s_latched |= ALL_CHANNELS;
    cooldown_start(ALL_CHANNELS, now_us);
    relay_trip();

    latch_publish();
    ESP_LOGE(TAG, "TOTAL OVERCURRENT TRIP at %.1f A (heat %.0f A²s): every relay latched off", amps,
             s_i2t_total.heat_a2s);
    for (int ch = 0; ch < CHANNELS; ch++) {
        shelly_onoff_set_async(s_endpoint_id[ch], false);
    }
}
#endif

static void trip_overvoltage(float volts, int64_t now_us)
{
    uint32_t was_on = relay_get_mask() & ALL_CHANNELS;
    s_ov_active = true;
    cooldown_start(ALL_CHANNELS, now_us);
    relay_trip();

    portENTER_CRITICAL(&s_restore_mux);
    s_restore_on = was_on;
    s_restore_pending = ALL_CHANNELS;
    portEXIT_CRITICAL(&s_restore_mux);
    shelly_fault_set(SHELLY_FAULT_SRC_OVERVOLTAGE, true);
    ESP_LOGE(TAG, "OVERVOLTAGE TRIP at %.0f V (limit %.0f V): relays off, on before: 0x%02x", volts, s_ov.limit_v,
             (unsigned)was_on);
    for (int ch = 0; ch < CHANNELS; ch++) {
        shelly_onoff_set_async(s_endpoint_id[ch], false);
    }
}

static void recover_overvoltage(void)
{
    s_ov_active = false;
    shelly_fault_set(SHELLY_FAULT_SRC_OVERVOLTAGE, false);
    portENTER_CRITICAL(&s_restore_mux);
    uint32_t restore = s_restore_pending & s_restore_on;
    s_restore_pending = 0;
    portEXIT_CRITICAL(&s_restore_mux);
    ESP_LOGW(TAG, "Voltage back in range for %ds%s", CONFIG_SHELLY_PROT_OV_RECOVER_S,
             restore ? ": restoring relays that were on" : "");
    for (int ch = 0; ch < CHANNELS; ch++) {
        if (restore & (1u << ch)) {
            // Goes through the attribute write and the full relay gate: a
            // latched overcurrent, over-temperature or cooldown still refuses it.
            shelly_onoff_set_async(s_endpoint_id[ch], true);
        }
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

    // Overcurrent, per relay. Heat builds only while the relay is closed:
    // once a trip opens it, the meter's RMS registers still read the
    // pre-trip current for up to one refresh, and counting that would push
    // the heat past the trip level for up to a minute, so an On long after
    // the 2 s cooldown would re-trip at the next poll whatever the load
    // draws. For the same reason the heat is capped at the trip level when
    // it trips.
    float total_a = 0.0f;
    for (int ch = 0; ch < CHANNELS; ch++) {
        float amps = sample->amps[ch];
        bool closed = relay_get_ch(ch);
        if (closed) {
            total_a += amps;
        }
        bool over = i2t_update(&s_i2t[ch], closed ? amps : 0.0f, dt_s);
        if (over && !(s_latched & (1u << ch))) {
            trip_overcurrent(ch, amps, now_us);
            s_i2t[ch].heat_a2s = I2T_K_A2S;
        }
        if (amps > PICKUP_A && !s_overcurrent_logged[ch]) {
            ESP_LOGW(TAG, "Relay %d: %.1f A above %.1f A pickup: trip in %.0fs at this level", ch + 1, amps, PICKUP_A,
                     i2t_trip_time_s(&s_i2t[ch], amps));
            s_overcurrent_logged[ch] = true;
        } else if (amps <= PICKUP_A) {
            s_overcurrent_logged[ch] = false;
        }
    }

#if HAS_TOTAL
    // The whole device: the sum of the closed relays' currents.
    bool total_over = i2t_update(&s_i2t_total, total_a, dt_s);
    if (total_over && (s_latched & ALL_CHANNELS) != ALL_CHANNELS) {
        trip_total(total_a, now_us);
        s_i2t_total.heat_a2s = TOTAL_K_A2S;
    }
    if (total_a > TOTAL_PICKUP_A && !s_total_logged) {
        ESP_LOGW(TAG, "Total %.1f A above %.1f A pickup: trip in %.0fs at this level", total_a, TOTAL_PICKUP_A,
                 i2t_trip_time_s(&s_i2t_total, total_a));
        s_total_logged = true;
    } else if (total_a <= TOTAL_PICKUP_A) {
        s_total_logged = false;
    }
#else
    (void)total_a;
#endif

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

    // Welded relay (alert only), shown while any relay is welded.
    bool weld_changed = false;
    bool any_welded = false;
    for (int ch = 0; ch < CHANNELS; ch++) {
        if (welded_update(&s_welded[ch], relay_get_ch(ch), sample->amps[ch], now_ms)) {
            weld_changed = true;
            if (s_welded[ch].alarmed) {
                ESP_LOGE(TAG, "RELAY %d WELDED? %.2f A flowing while commanded off", ch + 1, sample->amps[ch]);
            } else {
                ESP_LOGI(TAG, "Welded-relay alert cleared on relay %d", ch + 1);
            }
        }
        any_welded = any_welded || s_welded[ch].alarmed;
    }
    if (weld_changed) {
        shelly_fault_set(SHELLY_FAULT_SRC_WELDED_RELAY, any_welded);
    }
}

esp_err_t protection_init(void)
{
    for (int ch = 0; ch < CHANNELS; ch++) {
        i2t_init(&s_i2t[ch], PICKUP_A, I2T_K_A2S, I2T_TAU_S);
        welded_init(&s_welded[ch], WELD_ALARM_A, WELD_CLEAR_A, WELD_SETTLE_MS, WELD_ALARM_MS, WELD_CLEAR_MS);
    }
#if HAS_TOTAL
    i2t_init(&s_i2t_total, TOTAL_PICKUP_A, TOTAL_K_A2S, I2T_TAU_S);
#endif
    nominal_classifier_init(&s_classifier, NOMINAL_PRESENT_V, NOMINAL_SETTLE, NOMINAL_SPLIT_V);
    s_last_valid_ms = esp_timer_get_time() / 1000;

    nominal_load();
    nominal_apply();

    uint32_t latched = rtc_latch_load();
    if (latched != 0) {
        s_latched = latched;
        shelly_fault_set(SHELLY_FAULT_SRC_OVERCURRENT, true);
        ESP_LOGW(TAG, "Overcurrent trip still latched after a software reset (relays 0x%02x): they stay off",
                 (unsigned)latched);
    }

    ESP_LOGI(TAG, "Overcurrent: pickup %.1f A, k %.0f A²s (trip %.0fs at %.1f A, %.1fs at %.1f A)",
             PICKUP_A, I2T_K_A2S, i2t_trip_time_s(&s_i2t[0], RATED_A * 1.2f), RATED_A * 1.2f,
             i2t_trip_time_s(&s_i2t[0], RATED_A * 2.0f), RATED_A * 2.0f);
#if HAS_TOTAL
    ESP_LOGI(TAG, "Total current: pickup %.1f A, k %.0f A²s (trip %.0fs at %.1f A)", TOTAL_PICKUP_A, TOTAL_K_A2S,
             i2t_trip_time_s(&s_i2t_total, TOTAL_RATED_A * 1.2f), TOTAL_RATED_A * 1.2f);
#endif
    return relay_register_gate(protection_gate);
}

#endif // CONFIG_SHELLY_HAS_METER
