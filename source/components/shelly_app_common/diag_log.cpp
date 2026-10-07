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

// diag_log.cpp
//
// The optional diagnostics log (CONFIG_SHELLY_DIAG_LOG). Built to stay out
// of the way of the device:
// - one static ring in LP RAM (RTC_NOINIT), no allocation at runtime;
// - the log hook uses a 100-byte stack buffer and a spinlock around a
//   memcpy, and still sends every line to the UART unchanged;
// - status lines run on the Matter thread, taking the Thread lock the way
//   the Matter SDK does;
// - the maintenance boot never installs the hook: the log stays as the
//   failed pairing left it, for the page to show.

#include "shelly_diag_log.h"

#if CONFIG_SHELLY_DIAG_LOG

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <esp_attr.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_netif.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>

#include <esp_openthread.h>
#include <esp_openthread_netif_glue.h>
#include <openthread/icmp6.h>
#include <openthread/ip6.h>
#include <openthread/thread.h>

#include <app/server/Server.h>
#include <esp_matter.h> // The commissioning session and window events
#include <platform/CHIPDeviceEvent.h>
#include <platform/PlatformManager.h>
#include <platform/ThreadStackManager.h>

#include <diag_ring.h>

static const char *TAG = "diag";

#define LINE_MAX_BYTES      100
#define FREEZE_DELAY_S      5
#define STATUS_FAST_S       5
#define STATUS_SLOW_S       60

static RTC_NOINIT_ATTR diag_ring_t s_ring;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static vprintf_like_t s_uart_vprintf = nullptr;
static bool s_capturing = false;

// Seconds since boot, as 32-bit words: written by the Matter thread, read by
// the health task, so they must not tear.
static volatile uint32_t s_freeze_at_s = 0;   // 0 = none pending
static volatile uint32_t s_fast_until_s = 0;  // Status every 5 s until then (pairing activity)

static uint32_t now_s(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000000);
}
static uint32_t s_ticks = 0;
static volatile bool s_status_busy = false;
static uint32_t s_ot_addr_hash = 0;
static uint32_t s_ip_addr_hash = 0;

static void ring_append(const char *bytes, size_t n)
{
    portENTER_CRITICAL(&s_mux);
    diag_ring_append(&s_ring, bytes, n);
    portEXIT_CRITICAL(&s_mux);
}

// Every esp_log line comes through here. Matter prints its own "I (t) tag: "
// prefix straight to stdout and passes only the message, with no newline:
// those get a timestamp and a newline here.
static int log_hook(const char *fmt, va_list args)
{
    char line[LINE_MAX_BYTES];
    va_list copy;
    va_copy(copy, args);
    int n = vsnprintf(line, sizeof(line), fmt, copy);
    va_end(copy);
    if (n > 0) {
        size_t len = (size_t)n < sizeof(line) - 1 ? (size_t)n : sizeof(line) - 1;
        bool prefixed = len >= 3 && line[1] == ' ' && line[2] == '(';
        if (!prefixed) {
            char pre[20];
            int p = snprintf(pre, sizeof(pre), "c (%lu) ", (unsigned long)esp_log_timestamp());
            ring_append(pre, p > 0 ? (size_t)p : 0);
        }
        ring_append(line, len);
        if (line[len - 1] != '\n') {
            ring_append("\n", 1);
        }
    }
    return s_uart_vprintf != nullptr ? s_uart_vprintf(fmt, args) : vprintf(fmt, args);
}

void shelly_diag_log_start(void)
{
    // Keep the log across a crash or watchdog reset: that is when it matters.
    esp_reset_reason_t why = esp_reset_reason();
    bool crashed = why == ESP_RST_PANIC || why == ESP_RST_INT_WDT || why == ESP_RST_TASK_WDT || why == ESP_RST_WDT;
    portENTER_CRITICAL(&s_mux);
    diag_ring_init(&s_ring, crashed);
    portEXIT_CRITICAL(&s_mux);
    s_uart_vprintf = esp_log_set_vprintf(log_hook);
    s_capturing = true;
    s_fast_until_s = now_s() + 16 * 60; // The boot pairing window
    ESP_LOGW(TAG, "Diagnostics log on (boot %lu, reset reason %d%s%s)", (unsigned long)s_ring.boots, (int)why,
             s_ring.frozen ? ", frozen: " : "", s_ring.frozen ? s_ring.reason : "");
}

static uint32_t hash_bytes(uint32_t h, const void *p, size_t n)
{
    const uint8_t *b = (const uint8_t *)p;
    for (size_t i = 0; i < n; i++) {
        h = (h ^ b[i]) * 16777619u;
    }
    return h;
}

// On the Matter thread (8 KB stack).
static void status_work(intptr_t)
{
    s_status_busy = false;
    size_t free_b = heap_caps_get_free_size(MALLOC_CAP_DEFAULT);
    size_t min_b = heap_caps_get_minimum_free_size(MALLOC_CAP_DEFAULT);
    size_t big_b = heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT);
    chip::Server &server = chip::Server::GetInstance();
    bool failsafe = server.GetFailSafeContext().IsFailSafeArmed();
    bool window = server.GetCommissioningWindowManager().IsCommissioningWindowOpen();

    otInstance *ot = esp_openthread_get_instance();
    if (ot == nullptr) {
        ESP_LOGI(TAG, "heap %u min %u big %u fs %d win %d no Thread", (unsigned)free_b, (unsigned)min_b,
                 (unsigned)big_b, failsafe, window);
        return;
    }
    // The Matter SDK's own lock order (Matter lock, then Thread lock), from
    // this same thread. Not esp_openthread_lock_acquire(0): when it fails
    // half-way it keeps one of its two mutexes, which would stop Thread.
    chip::DeviceLayer::ThreadStackMgr().LockThreadStack();
    // Ping on every address is on in every build (app_common.cpp).
    int echo = (int)otIcmp6GetEchoMode(ot);
    otDeviceRole role = otThreadGetDeviceRole(ot);
    uint16_t rloc = otThreadGetRloc16(ot);
    uint32_t part = otThreadGetPartitionId(ot);
    char ot_addrs[8][OT_IP6_ADDRESS_STRING_SIZE];
    int n_ot = 0;
    uint32_t h = 2166136261u;
    for (const otNetifAddress *a = otIp6GetUnicastAddresses(ot); a != nullptr; a = a->mNext) {
        h = hash_bytes(h, &a->mAddress, sizeof(a->mAddress));
        if (n_ot < 8) {
            otIp6AddressToString(&a->mAddress, ot_addrs[n_ot++], OT_IP6_ADDRESS_STRING_SIZE);
        }
    }
    chip::DeviceLayer::ThreadStackMgr().UnlockThreadStack();

    ESP_LOGI(TAG, "heap %u min %u big %u fs %d win %d role %s rloc 0x%04x part %08lx echo %d", (unsigned)free_b,
             (unsigned)min_b, (unsigned)big_b, failsafe, window, otThreadDeviceRoleToString(role), rloc,
             (unsigned long)part, echo);
    if (h != s_ot_addr_hash) {
        s_ot_addr_hash = h;
        for (int i = 0; i < n_ot; i++) {
            ESP_LOGI(TAG, "ot addr %s", ot_addrs[i]);
        }
    }

    esp_netif_t *netif = esp_openthread_get_netif();
    if (netif != nullptr) {
        esp_ip6_addr_t ip[16];
        int n_ip = esp_netif_get_all_ip6(netif, ip);
        uint32_t h2 = hash_bytes(2166136261u, ip, (size_t)(n_ip > 0 ? n_ip : 0) * sizeof(ip[0]));
        if (h2 != s_ip_addr_hash) {
            s_ip_addr_hash = h2;
            ESP_LOGI(TAG, "lwip has %d addr", n_ip);
            for (int i = 0; i < n_ip && i < 16; i++) {
                ESP_LOGI(TAG, "lwip addr " IPV6STR, IPV62STR(ip[i]));
            }
        }
    }
}

void shelly_diag_log_tick(void)
{
    if (!s_capturing) {
        return;
    }
    uint32_t at = s_freeze_at_s;
    if (at != 0 && now_s() >= at) {
        s_freeze_at_s = 0;
        ESP_LOGW(TAG, "Log frozen: commissioning failed");
        portENTER_CRITICAL(&s_mux);
        diag_ring_freeze(&s_ring, "commissioning failed");
        portEXIT_CRITICAL(&s_mux);
    }
    s_ticks++;
    // The health task holds no Matter lock here: pairing activity is known
    // from the events instead (s_fast_until_s).
    uint32_t every = now_s() < s_fast_until_s ? STATUS_FAST_S : STATUS_SLOW_S;
    if (s_ticks % every == 0 && !s_status_busy) {
        s_status_busy = true;
        if (chip::DeviceLayer::PlatformMgr().ScheduleWork(status_work, 0) != CHIP_NO_ERROR) {
            s_status_busy = false;
        }
    }
}

void shelly_diag_log_event(int type)
{
    namespace ev = chip::DeviceLayer::DeviceEventType;
    if (!s_capturing) {
        return;
    }
    const char *name = nullptr;
    switch (type) {
    case ev::kCommissioningSessionStarted: name = "PASE session started"; break;
    case ev::kCommissioningSessionStopped: name = "PASE session stopped"; break;
    case ev::kCommissioningComplete: name = "COMMISSIONING COMPLETE"; break;
    case ev::kCommissioningWindowOpened: name = "pairing window opened"; break;
    case ev::kCommissioningWindowClosed: name = "pairing window closed"; break;
    case ev::kThreadStateChange: name = "Thread state changed"; break;
    case ev::kInterfaceIpAddressChanged: name = "IP address changed"; break;
    case ev::kFailSafeTimerExpired: name = "FAIL-SAFE EXPIRED: commissioning failed"; break;
    default: return;
    }
    ESP_LOGW(TAG, "== %s", name);
    // Pairing activity: status lines every 5 s for the next 10 minutes.
    s_fast_until_s = now_s() + 10 * 60;
    if (type == ev::kFailSafeTimerExpired && s_freeze_at_s == 0 && !s_ring.frozen) {
        s_freeze_at_s = now_s() + FREEZE_DELAY_S;
    }
}

bool shelly_diag_log_available(void)
{
    return diag_ring_valid(&s_ring);
}

size_t shelly_diag_log_header(char *buf, size_t len)
{
    if (!diag_ring_valid(&s_ring)) {
        return (size_t)snprintf(buf, len, "No diagnostics log (a power cut clears it).\n");
    }
    int n = snprintf(buf, len, "Shelly diagnostics log. Normal boots: %lu. %s%s\n--\n", (unsigned long)s_ring.boots,
                     s_ring.frozen ? "Frozen: " : "Not frozen (still recording when last running).",
                     s_ring.frozen ? s_ring.reason : "");
    return n > 0 ? (size_t)n : 0;
}

void shelly_diag_log_read(shelly_diag_span_fn fn, void *ctx)
{
    // In the maintenance boot nothing writes the ring, so no lock is needed
    // while the page streams it.
    if (diag_ring_valid(&s_ring)) {
        diag_ring_read(&s_ring, fn, ctx);
    }
}

void shelly_diag_log_clear(void)
{
    portENTER_CRITICAL(&s_mux);
    diag_ring_reset(&s_ring);
    portEXIT_CRITICAL(&s_mux);
}

#endif // CONFIG_SHELLY_DIAG_LOG
