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



// shelly_pulse.cpp

#include "shelly_pulse.h"

#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <sdkconfig.h>

#include <platform/PlatformManager.h>

#include <relay.h>

#include "shelly_app.h"

static const char *TAG = "pulse";

#ifndef CONFIG_SHELLY_PULSE_MS
#define CONFIG_SHELLY_PULSE_MS 500
#endif

// ---- Through Matter ----

// Where the pulse is. RELEASED: the relay opened on time but OnOff still
// reads On, until the Matter thread clears it. A local toggle in that gap
// would read the stale On and only clear it, so it clears it first
// (pulse_settle). s_gen counts pulses, so a late clear for an old pulse
// never ends a new one. HELD: a Follow-mode wall switch is on, so the
// relay stays closed past the pulse length until the switch turns off
// (s_hold), or a toggle or Matter Off ends it.
enum pulse_phase_t { PULSE_IDLE, PULSE_RUNNING, PULSE_HELD, PULSE_RELEASED };

static esp_timer_handle_t s_pulse_timer = nullptr;
static volatile uint16_t s_pulse_endpoint = 0;
static portMUX_TYPE s_pulse_mux = portMUX_INITIALIZER_UNLOCKED;
static pulse_phase_t s_phase = PULSE_IDLE;
static uint32_t s_gen = 0;
static bool s_hold = false;

// Retry interval when the Matter work queue is full.
#define PULSE_CLEAR_RETRY_MS 100

static void pulse_set_phase(pulse_phase_t phase)
{
    taskENTER_CRITICAL(&s_pulse_mux);
    s_phase = phase;
    taskEXIT_CRITICAL(&s_pulse_mux);
}

static void pulse_set_hold(bool hold)
{
    taskENTER_CRITICAL(&s_pulse_mux);
    s_hold = hold;
    taskEXIT_CRITICAL(&s_pulse_mux);
}

// On the Matter thread: clear OnOff for this pulse, unless it was already
// cleared or a new pulse has started since.
static void pulse_clear_work(intptr_t arg)
{
    taskENTER_CRITICAL(&s_pulse_mux);
    bool stale = s_phase == PULSE_RELEASED && s_gen == (uint32_t)arg;
    taskEXIT_CRITICAL(&s_pulse_mux);
    if (stale) {
        // Runs shelly_pulse_write(false), which sets PULSE_IDLE.
        shelly_onoff_set(s_pulse_endpoint, false);
    }
}

// Schedule the OnOff clear for a released pulse. A full queue is retried
// through the timer, so OnOff never stays On.
static void pulse_schedule_clear(uint32_t gen)
{
    // Logged once per pulse, not at every retry.
    static uint32_t s_warned_gen = 0;
    CHIP_ERROR err = chip::DeviceLayer::PlatformMgr().ScheduleWork(pulse_clear_work, (intptr_t)gen);
    if (err != CHIP_NO_ERROR) {
        if (s_warned_gen != gen) {
            s_warned_gen = gen;
            ESP_LOGW(TAG, "OnOff clear not scheduled, retrying every %d ms", PULSE_CLEAR_RETRY_MS);
        }
        esp_timer_start_once(s_pulse_timer, (uint64_t)PULSE_CLEAR_RETRY_MS * 1000);
    }
}

static void pulse_timer_cb(void *arg)
{
    taskENTER_CRITICAL(&s_pulse_mux);
    bool running = s_phase == PULSE_RUNNING;
    if (running && s_hold) {
        // The wall switch is still on: keep the relay closed.
        s_phase = PULSE_HELD;
        running = false;
    }
    uint32_t gen = s_gen;
    taskEXIT_CRITICAL(&s_pulse_mux);
    if (running) {
        // Release the trigger here, on time: the attribute update waits for
        // the CHIP stack lock, which the Matter thread can hold for seconds,
        // and it runs on the Matter thread instead of blocking the esp_timer
        // task.
        relay_set(false);
        taskENTER_CRITICAL(&s_pulse_mux);
        if (s_phase == PULSE_RUNNING && s_gen == gen) {
            s_phase = PULSE_RELEASED;
        }
        taskEXIT_CRITICAL(&s_pulse_mux);
    }
    taskENTER_CRITICAL(&s_pulse_mux);
    bool released = s_phase == PULSE_RELEASED;
    gen = s_gen;
    taskEXIT_CRITICAL(&s_pulse_mux);
    if (!released) {
        return;
    }
    // Matter not up can't happen here: a pulse needs it.
    pulse_schedule_clear(gen);
}

static esp_err_t pulse_timer_arm(void)
{
    if (s_pulse_timer == nullptr) {
        const esp_timer_create_args_t args = {
            .callback = pulse_timer_cb,
            .name = "pulse",
        };
        esp_err_t err = esp_timer_create(&args, &s_pulse_timer);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to create the pulse timer");
            return err;
        }
    }
    // Restart cleanly if a pulse is still running. Stopping a timer that
    // isn't running fails as expected: only the start decides.
    esp_timer_stop(s_pulse_timer);
    return esp_timer_start_once(s_pulse_timer, (uint64_t)CONFIG_SHELLY_PULSE_MS * 1000);
}

esp_err_t shelly_pulse_write(uint16_t endpoint_id, bool on)
{
    if (!on) {
        if (s_pulse_timer != nullptr) {
            esp_timer_stop(s_pulse_timer);
        }
        taskENTER_CRITICAL(&s_pulse_mux);
        s_phase = PULSE_IDLE;
        s_hold = false;
        taskEXIT_CRITICAL(&s_pulse_mux);
        return relay_set(false);
    }
    // A StartUpOnOff of On or Toggle is applied inside esp_matter::start()
    // through this same write, and would move the door at every boot.
    if (!shelly_app_is_started()) {
        ESP_LOGW(TAG, "Trigger refused: Matter is not up yet");
        return ESP_ERR_INVALID_STATE;
    }
    s_pulse_endpoint = endpoint_id;
    taskENTER_CRITICAL(&s_pulse_mux);
    s_phase = PULSE_RUNNING;
    s_gen++;
    taskEXIT_CRITICAL(&s_pulse_mux);
    esp_err_t err = pulse_timer_arm();
    if (err != ESP_OK) {
        pulse_set_phase(PULSE_IDLE);
        ESP_LOGE(TAG, "Pulse release unavailable, relay not closed");
        return err;
    }
    err = relay_set(true);
    if (err != ESP_OK) {
        // Refused (over-temperature): no stray release later.
        esp_timer_stop(s_pulse_timer);
        pulse_set_phase(PULSE_IDLE);
    }
    return err;
}

// With the CHIP stack lock held: if the last pulse has ended but OnOff
// still reads On, clear it now, so the toggle that follows sees Off.
static void pulse_settle(uint16_t endpoint_id)
{
    taskENTER_CRITICAL(&s_pulse_mux);
    bool released = s_phase == PULSE_RELEASED;
    taskEXIT_CRITICAL(&s_pulse_mux);
    if (released) {
        shelly_onoff_set(endpoint_id, false);
    }
}

bool shelly_pulse_toggle(uint16_t endpoint_id)
{
    ShellyInputLock lock(SHELLY_INPUT_LOCK_TIMEOUT_MS);
    if (!lock.locked()) {
        ESP_LOGW(TAG, "Matter busy for %d ms: toggle refused", SHELLY_INPUT_LOCK_TIMEOUT_MS);
        return false;
    }
    pulse_settle(endpoint_id);
    return shelly_onoff_toggle(endpoint_id);
}

// The switch turned off: open a held relay now, without the CHIP stack
// lock, so a busy Matter thread never keeps the door's button pressed. A
// pulse still inside its length ends on time by itself.
static bool pulse_hold_release(void)
{
    taskENTER_CRITICAL(&s_pulse_mux);
    s_hold = false;
    bool held = s_phase == PULSE_HELD;
    uint32_t gen = s_gen;
    taskEXIT_CRITICAL(&s_pulse_mux);
    if (!held) {
        return true;
    }
    // While held, OnOff reads On, so nothing else can start a pulse; only
    // an Off, which also opens the relay, can get in first.
    esp_err_t err = relay_set(false);
    taskENTER_CRITICAL(&s_pulse_mux);
    bool released = s_phase == PULSE_HELD && s_gen == gen;
    if (released) {
        s_phase = PULSE_RELEASED;
    }
    taskEXIT_CRITICAL(&s_pulse_mux);
    if (released) {
        pulse_schedule_clear(gen);
    }
    return err == ESP_OK;
}

bool shelly_pulse_toggle_to(uint16_t endpoint_id, bool on)
{
    if (!on) {
        return pulse_hold_release();
    }
    ShellyInputLock lock(SHELLY_INPUT_LOCK_TIMEOUT_MS);
    if (!lock.locked()) {
        ESP_LOGW(TAG, "Matter busy for %d ms: toggle refused", SHELLY_INPUT_LOCK_TIMEOUT_MS);
        return false;
    }
    pulse_settle(endpoint_id);
    // Set before the write, which starts the pulse; a pulse already
    // running (from Matter or the button) is held too.
    pulse_set_hold(true);
    bool ok = shelly_onoff_toggle_to(endpoint_id, true);
    if (!ok) {
        pulse_set_hold(false);
    }
    return ok;
}

esp_err_t shelly_pulse_set_defaults(uint16_t endpoint_id)
{
    esp_err_t err = relay_set(false);
    shelly_onoff_set(endpoint_id, false);
    return err;
}

// ---- Without Matter (Wi-Fi maintenance mode) ----

// The button task and the page's Toggle (the web server task) both call
// these, so they run under a lock.
static esp_timer_handle_t s_local_timer = nullptr;
static SemaphoreHandle_t s_local_lock = nullptr;
static StaticSemaphore_t s_local_lock_buf;

// Follow mode's hold, as with Matter: s_local_hold while the wall switch is
// on, s_local_ended once the pulse length has passed. The relay opens at
// the end of the pulse unless held, or when the switch turns off after it.
static portMUX_TYPE s_local_mux = portMUX_INITIALIZER_UNLOCKED;
static bool s_local_hold = false;
static bool s_local_ended = true;

static void local_set(bool hold, bool ended)
{
    taskENTER_CRITICAL(&s_local_mux);
    s_local_hold = hold;
    s_local_ended = ended;
    taskEXIT_CRITICAL(&s_local_mux);
}

static void local_pulse_end(void *arg)
{
    taskENTER_CRITICAL(&s_local_mux);
    s_local_ended = true;
    bool release = !s_local_hold;
    taskEXIT_CRITICAL(&s_local_mux);
    if (release) {
        relay_set(false);
    }
}

esp_err_t shelly_pulse_local_init(void)
{
    s_local_lock = xSemaphoreCreateMutexStatic(&s_local_lock_buf);
    const esp_timer_create_args_t args = {
        .callback = local_pulse_end,
        .name = "pulse_local",
    };
    esp_err_t err = esp_timer_create(&args, &s_local_timer);
    if (err != ESP_OK) {
        s_local_timer = nullptr;
        ESP_LOGE(TAG, "Failed to create the local pulse timer: the trigger is refused");
    }
    return err;
}

// With s_local_lock held: start a pulse, held or not, arming the release
// first.
static bool local_start(bool hold)
{
    // A pulse cut short by the relay (over-temperature) leaves its timer
    // armed; stopping one that isn't armed is harmless.
    esp_timer_stop(s_local_timer);
    local_set(hold, false);
    if (esp_timer_start_once(s_local_timer, (uint64_t)CONFIG_SHELLY_PULSE_MS * 1000) != ESP_OK) {
        local_set(false, true);
        return false;
    }
    if (relay_set(true) != ESP_OK) {
        esp_timer_stop(s_local_timer);
        local_set(false, true);
        return false;
    }
    return true;
}

// With s_local_lock held: end a pulse, held or not.
static bool local_stop(void)
{
    esp_timer_stop(s_local_timer);
    local_set(false, true);
    return relay_set(false) == ESP_OK;
}

bool shelly_pulse_local(void)
{
    if (s_local_timer == nullptr) {
        return false;
    }
    xSemaphoreTake(s_local_lock, portMAX_DELAY);
    bool ok = relay_get() ? local_stop() : local_start(false);
    xSemaphoreGive(s_local_lock);
    return ok;
}

bool shelly_pulse_local_to(bool on)
{
    if (s_local_timer == nullptr) {
        return false;
    }
    xSemaphoreTake(s_local_lock, portMAX_DELAY);
    bool ok = true;
    taskENTER_CRITICAL(&s_local_mux);
    // On: hold a pulse that is running or held; off: let go, and open the
    // relay unless the pulse length is still running (it opens on time).
    bool active = !s_local_ended || s_local_hold;
    bool release = s_local_ended;
    if (on && active) {
        s_local_hold = true;
    } else if (!on) {
        s_local_hold = false;
    }
    taskEXIT_CRITICAL(&s_local_mux);
    if (on && !active) {
        ok = local_start(true);
    } else if (!on && release && relay_get()) {
        ok = relay_set(false) == ESP_OK;
    }
    xSemaphoreGive(s_local_lock);
    return ok;
}
