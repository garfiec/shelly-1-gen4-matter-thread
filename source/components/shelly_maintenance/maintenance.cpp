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

// maintenance.cpp
//
// Wi-Fi maintenance mode. Runs in its own boot, with Matter, Thread and BLE
// never started: Espressif does not support a Wi-Fi access point beside a
// Thread router on the ESP32-C6 (api-guides/coexist.rst), and a boot of its
// own leaves the whole heap to Wi-Fi and the web server.

#include "maintenance.h"
#include "maintenance_dns.h"
#include "maintenance_image.h"
#include "qr_svg.h"

#include <stdlib.h>
#include <string.h>

#include <esp_app_desc.h>
#include <esp_event.h>
#include <esp_http_server.h>
#include <esp_log.h>
#include <esp_mac.h>
#include <esp_netif.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <esp_system.h>
#include <esp_task_wdt.h>
#include <esp_timer.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <dhcpserver/dhcpserver.h>
#include <lwip/sockets.h>
#include <sdkconfig.h>

#include <math.h>

#include <metering_live.h>
#include <relay.h>
#include <shelly_calibration.h>
#include <shelly_hw_desc.h>
#include <shelly_hw_snapshot.h>
#include <shelly_diag_log.h>
#include <shelly_settings.h>
#include <shelly_setup_code.h>
#include <status_led.h>

static const char *TAG = "maintenance";

#if CONFIG_SHELLY_HW_SNAPSHOT
#define HW_DATA_BUILT true
#else
#define HW_DATA_BUILT false
#endif
#if CONFIG_SHELLY_FACTORY_DUMP
#define FACTORY_DUMP_BUILT true
#else
#define FACTORY_DUMP_BUILT false
#endif

#define AP_IP_0             192
#define AP_IP_1             168
#define AP_IP_2             33
#define AP_IP_3             1
#define AP_CHANNEL          1
#define AP_MAX_STATIONS     2
#define IDLE_TIMEOUT_MS     ((uint32_t)CONFIG_SHELLY_MAINTENANCE_IDLE_TIMEOUT_S * 1000u)
#define RESTART_DELAY_MS    1500
#define UPLOAD_CHUNK        4096
#define HTTP_STACK_SIZE     6144
#define DNS_STACK_SIZE      3072
// The largest upload accepted: an app slot plus a generous .ota header.
#define MAX_UPLOAD_BYTES    (0x300000 + MI_OTA_FIXED_HEADER + MI_OTA_MAX_TLV)

extern const char page_html_start[] asm("_binary_page_html_start");
extern const char page_html_end[] asm("_binary_page_html_end");

static shelly_ui_toggle_fn_t s_local_toggle = nullptr;
static bool (*s_local_toggle_ch)(int ch) = nullptr;
// Milliseconds since boot, 32-bit: the web server and event tasks write it
// and the main loop reads it, and a 64-bit value could tear on this core.
static volatile uint32_t s_last_activity_ms = 0;
static volatile bool s_uploading = false;
static volatile bool s_restart = false;
static char s_mac[18];

static uint32_t uptime_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static void touch(void)
{
    s_last_activity_ms = uptime_ms();
}

// Every clean way out of the mode into normal operation (a failed start
// uses shelly_maintenance_failed_record() instead): the normal boot keeps
// the relay's state here as its previous state (see shelly_app.h).
[[noreturn]] static void restart_to_normal(void)
{
    shelly_maintenance_exit_record();
    esp_restart();
}

// A copy of a string from an image that is safe inside a JSON string: the
// file is not trusted, and send_result() adds no escaping.
static void json_safe(char *dst, size_t len, const char *src)
{
    size_t n = 0;
    for (; n + 1 < len && *src != '\0'; src++) {
        unsigned char c = (unsigned char)*src;
        dst[n++] = (c < 0x20 || c >= 0x7F || c == '"' || c == '\\') ? '?' : (char)c;
    }
    dst[n] = '\0';
}

// ---------------------------------------------------------------------------
// Wi-Fi access point
// ---------------------------------------------------------------------------

static void wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (id == WIFI_EVENT_AP_STACONNECTED) {
        ESP_LOGI(TAG, "A device joined the maintenance network");
    } else if (id == WIFI_EVENT_AP_STADISCONNECTED) {
        ESP_LOGI(TAG, "A device left the maintenance network");
    }
    touch();
}

// The model as stock names it in its network name: the hardware code without
// its leading S (S2PMG4 -> 2PMG4), and 1MiniG4 for the Mini.
static const char *stock_model_name()
{
    const char *code = shelly_hw_desc.hw_code;
    if (strcmp(code, "Mini1G4") == 0) {
        return "1MiniG4";
    }
    return code[0] == 'S' ? code + 1 : code;
}

static esp_err_t start_wifi(void)
{
    esp_err_t err = esp_netif_init();
    if (err != ESP_OK) {
        return err;
    }
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }
    esp_netif_t *ap = esp_netif_create_default_wifi_ap();
    if (ap == nullptr) {
        return ESP_FAIL;
    }

    // Stock Shelly firmware serves its setup page at 192.168.33.1.
    esp_netif_ip_info_t ip = {};
    IP4_ADDR(&ip.ip, AP_IP_0, AP_IP_1, AP_IP_2, AP_IP_3);
    IP4_ADDR(&ip.gw, AP_IP_0, AP_IP_1, AP_IP_2, AP_IP_3);
    IP4_ADDR(&ip.netmask, 255, 255, 255, 0);
    esp_netif_dhcps_stop(ap);
    err = esp_netif_set_ip_info(ap, &ip);
    if (err != ESP_OK) {
        return err;
    }
    // Hand out the device itself as the DNS server, for the captive portal.
    esp_netif_dns_info_t dns = {};
    dns.ip.type = ESP_IPADDR_TYPE_V4;
    dns.ip.u_addr.ip4.addr = ip.ip.addr;
    esp_netif_set_dns_info(ap, ESP_NETIF_DNS_MAIN, &dns);
    dhcps_offer_t offer_dns = OFFER_DNS;
    esp_netif_dhcps_option(ap, ESP_NETIF_OP_SET, ESP_NETIF_DOMAIN_NAME_SERVER, &offer_dns, sizeof(offer_dns));
    err = esp_netif_dhcps_start(ap);
    if (err != ESP_OK && err != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STARTED) {
        return err;
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&cfg);
    if (err != ESP_OK) {
        return err;
    }
    // Nothing about this network is stored: it exists only for this boot.
    esp_wifi_set_storage(WIFI_STORAGE_RAM);
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event, nullptr);

    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    snprintf(s_mac, sizeof(s_mac), "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    // Stock Shelly's name for its setup network: the model and the base MAC
    // printed on the sticker, e.g. Shelly2PMG4-AABBCCDDEEFF.
    uint8_t base[6] = {};
    esp_efuse_mac_get_default(base);
    wifi_config_t ap_cfg = {};
    int n = snprintf((char *)ap_cfg.ap.ssid, sizeof(ap_cfg.ap.ssid), "Shelly%s-%02X%02X%02X%02X%02X%02X",
                     stock_model_name(), base[0], base[1], base[2], base[3], base[4], base[5]);
    ap_cfg.ap.ssid_len = (uint8_t)(n < (int)sizeof(ap_cfg.ap.ssid) ? n : (int)sizeof(ap_cfg.ap.ssid));
    ap_cfg.ap.channel = AP_CHANNEL;
    ap_cfg.ap.authmode = WIFI_AUTH_OPEN;
    ap_cfg.ap.max_connection = AP_MAX_STATIONS;

    err = esp_wifi_set_mode(WIFI_MODE_AP);
    if (err == ESP_OK) {
        err = esp_wifi_set_config(WIFI_IF_AP, &ap_cfg);
    }
    if (err == ESP_OK) {
        err = esp_wifi_start();
    }
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Maintenance network \"%s\" up at 192.168.33.1", (const char *)ap_cfg.ap.ssid);
    }
    return err;
}

// ---------------------------------------------------------------------------
// Captive-portal DNS: every name is this device
// ---------------------------------------------------------------------------

static void dns_task(void *arg)
{
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        ESP_LOGE(TAG, "DNS socket failed");
        vTaskDelete(nullptr);
        return;
    }
    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(53);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "DNS bind failed");
        close(sock);
        vTaskDelete(nullptr);
        return;
    }
    static const uint8_t ip[4] = {AP_IP_0, AP_IP_1, AP_IP_2, AP_IP_3};
    uint8_t query[256];
    uint8_t reply[256 + 16];
    while (true) {
        struct sockaddr_in from = {};
        socklen_t from_len = sizeof(from);
        int len = recvfrom(sock, query, sizeof(query), 0, (struct sockaddr *)&from, &from_len);
        if (len < 0) {
            // A socket that keeps failing must not spin: this task outranks
            // the main loop that feeds the task watchdog.
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        if (len == 0) {
            continue;
        }
        size_t out = dns_captive_reply(query, (size_t)len, ip, reply, sizeof(reply));
        if (out > 0) {
            sendto(sock, reply, out, 0, (struct sockaddr *)&from, from_len);
        }
    }
}

// ---------------------------------------------------------------------------
// Web page
// ---------------------------------------------------------------------------

static esp_err_t send_json(httpd_req_t *req, const char *status, const char *json)
{
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req, json);
}

// A JSON reply with a message; the message must not contain quotes or
// backslashes (all of ours are fixed text).
static esp_err_t send_result(httpd_req_t *req, const char *status, bool ok, const char *msg)
{
    char body[320];
    snprintf(body, sizeof(body), "{\"ok\":%s,\"msg\":\"%s\"}", ok ? "true" : "false", msg);
    return send_json(req, status, body);
}

static esp_err_t page_get(httpd_req_t *req)
{
    touch();
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, page_html_start, page_html_end - page_html_start - 1);
}

static esp_err_t status_get(httpd_req_t *req)
{
    touch();
    const esp_app_desc_t *app = esp_app_get_description();
    char project[sizeof(app->project_name) + 1];
    char version[sizeof(app->version) + 1];
    json_safe(project, sizeof(project), app->project_name);
    json_safe(version, sizeof(version), app->version);

    // The setup code in use, and a change waiting for 3 presses.
    setup_code_t code;
    bool created = false;
    bool saved = shelly_setup_code_current(&code, &created);
    char code_text[SETUP_CODE_FORMATTED_LEN];
    setup_code_manual_format(code.passcode, code.discriminator, code_text);
    const char *source = !saved                                     ? "test"
                         : code.source == SETUP_CODE_SOURCE_USER    ? "yours"
                         : code.source == SETUP_CODE_SOURCE_STICKER ? "sticker"
                                                                    : "generated";
    // The sticker's code, shown beside another code in use.
    setup_code_t sticker;
    char sticker_text[SETUP_CODE_FORMATTED_LEN + 2] = "null";
    if (shelly_setup_code_sticker(&sticker)) {
        char t[SETUP_CODE_FORMATTED_LEN];
        setup_code_manual_format(sticker.passcode, sticker.discriminator, t);
        snprintf(sticker_text, sizeof(sticker_text), "\"%s\"", t);
    }
    bool pending_sticker = false;
    shelly_change_t change;
    uint32_t secs_left = 0;
    const char *pending_kind = "null";
    char pending[SETUP_CODE_FORMATTED_LEN + 2] = "null";
    if (shelly_change_pending(&change, &secs_left)) {
        if (change.kind == SHELLY_CHANGE_SETUP_CODE) {
            char t[SETUP_CODE_FORMATTED_LEN];
            setup_code_manual_format(change.code.passcode, change.code.discriminator, t);
            snprintf(pending, sizeof(pending), "\"%s\"", t);
            pending_kind = "\"code\"";
            pending_sticker = change.code.source == SETUP_CODE_SOURCE_STICKER;
        } else if (change.kind == SHELLY_CHANGE_CALIBRATION) {
            snprintf(pending, sizeof(pending), "\"%s\"", cal_ratios_is_default(&change.cal) ? "default" : "new");
            pending_kind = "\"cal\"";
        } else {
            snprintf(pending, sizeof(pending), "\"%s\"", ui_switch_mode_name(change.switch_mode));
            pending_kind = "\"switch\"";
        }
    }
    bool has_modes = shelly_ui_switch_role() == UI_SWITCH_TOGGLE;
    bool channel_kind = strcmp(pending_kind, "\"switch\"") == 0 || strcmp(pending_kind, "\"cal\"") == 0;
    unsigned pending_ch = channel_kind ? change.channel + 1u : 0u;

    char body[1024];
    int n = snprintf(body, sizeof(body),
                     "{\"hw\":\"%s\",\"variant\":\"%s\",\"version\":\"%s\",\"mac\":\"%s\",\"relay\":%s,"
                     "\"code\":\"%s\",\"code_source\":\"%s\",\"code_new\":%s,"
                     "\"switch_modes\":%s,\"switch_mode\":\"%s\","
                     "\"pending_kind\":%s,\"pending\":%s,\"pending_secs\":%u,\"diag\":%s,"
                     "\"channels\":%d,\"relay2\":%s,\"switch_mode2\":\"%s\",\"pending_ch\":%u,\"hw_data\":%s,"
                     "\"meter\":%s,\"meter_skipped\":%s,\"parts\":%s,\"sticker\":%s,\"pending_sticker\":%s}",
                     shelly_hw_desc.hw_code, project, version, s_mac, relay_get() ? "true" : "false", code_text,
                     source, created ? "true" : "false", has_modes ? "true" : "false",
                     ui_switch_mode_name(shelly_switch_mode_get()), pending_kind, pending, (unsigned)secs_left,
                     shelly_diag_log_available() ? "true" : "false", RELAY_CHANNELS,
                     relay_get_ch(1) ? "true" : "false", ui_switch_mode_name(shelly_switch_mode_get_ch(1)),
                     pending_ch, HW_DATA_BUILT ? "true" : "false", shelly_cal_available() ? "true" : "false",
                     shelly_maintenance_meter_skipped() ? "true" : "false",
                     FACTORY_DUMP_BUILT ? "true" : "false", sticker_text, pending_sticker ? "true" : "false");
    if (n < 0 || n >= (int)sizeof(body)) {
        return send_result(req, "500 Internal Server Error", false, "Status too long");
    }
    return send_json(req, "200 OK", body);
}

static esp_err_t exit_post(httpd_req_t *req)
{
    ESP_LOGI(TAG, "Exit requested from the page");
    send_result(req, "200 OK", true, "Restarting");
    s_restart = true;
    return ESP_OK;
}

// Any other address sends the browser to the page. Phones probe a known URL
// to detect a captive portal; the redirect is what makes them open it.
static esp_err_t redirect_404(httpd_req_t *req, httpd_err_code_t err)
{
    // Not activity: a phone that stays joined re-probes these every few
    // minutes, which must not keep the mode alive. Opening the page (GET /)
    // and using it are what count.
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://192.168.33.1/");
    return httpd_resp_send(req, nullptr, 0);
}

static int recv_some(httpd_req_t *req, uint8_t *buf, size_t len)
{
    for (int tries = 0; tries < 3; tries++) {
        int n = httpd_req_recv(req, (char *)buf, len);
        if (n != HTTPD_SOCK_ERR_TIMEOUT) {
            return n;
        }
    }
    return HTTPD_SOCK_ERR_TIMEOUT;
}

// A short POST body, read in full and NUL-terminated. False (and a reply
// sent) if it is empty, too long or does not arrive.
static bool read_small_body(httpd_req_t *req, char *text, size_t size, esp_err_t *reply)
{
    if (req->content_len == 0 || req->content_len >= size) {
        *reply = send_result(req, "400 Bad Request", false, "Nothing to save");
        return false;
    }
    size_t got = 0;
    while (got < req->content_len) {
        int n = recv_some(req, (uint8_t *)text + got, req->content_len - got);
        if (n <= 0) {
            *reply = send_result(req, "400 Bad Request", false, "The request did not arrive; try again");
            return false;
        }
        got += (size_t)n;
    }
    text[got] = '\0';
    return true;
}

// POST /toggle: toggle relay 1, or with the body "2" relay 2.
static esp_err_t toggle_post(httpd_req_t *req)
{
    touch();
    int ch = 0;
    if (req->content_len > 0) {
        char text[4];
        esp_err_t reply = ESP_OK;
        if (!read_small_body(req, text, sizeof(text), &reply)) {
            return reply;
        }
        ch = atoi(text) - 1;
        if (ch < 0 || ch >= RELAY_CHANNELS) {
            return send_result(req, "400 Bad Request", false, "This device has no such relay");
        }
    }
    bool ok;
    if (s_local_toggle_ch != nullptr) {
        ok = s_local_toggle_ch(ch);
    } else {
        ok = ch == 0 && s_local_toggle != nullptr && s_local_toggle();
    }
    if (!ok) {
        status_led_flash_refused();
    }
    return send_result(req, "200 OK", ok, ok ? "" : "The relay refused to switch (over-temperature or protection)");
}

// POST /code/set: the owner's 11-digit code, as typed. It waits for 3
// presses of the button (docs/BUTTON-AND-LED.md, B-26) before it is saved.
static esp_err_t code_set_post(httpd_req_t *req)
{
    touch();
    char text[40];
    esp_err_t reply = ESP_OK;
    if (!read_small_body(req, text, sizeof(text), &reply)) {
        return reply;
    }
    switch (shelly_setup_code_stage_manual(text)) {
    case SETUP_CODE_OK:
        return send_result(req, "200 OK", true, "");
    case SETUP_CODE_ERR_CHECK_DIGIT:
        return send_result(req, "400 Bad Request", false, "That code has a typo: check each digit");
    case SETUP_CODE_ERR_LONG_FORM:
        return send_result(req, "400 Bad Request", false,
                           "That is the long 21-digit form. Use the 11-digit code (XXXX-XXX-XXXX)");
    case SETUP_CODE_ERR_PASSCODE:
        return send_result(req, "400 Bad Request", false, "Matter does not allow that code");
    case SETUP_CODE_ERR_FORMAT:
    default:
        return send_result(req, "400 Bad Request", false, "Enter the 11-digit code, like 1234-567-8901");
    }
}

// GET /qr.svg: the QR of the setup code in use. The sticker's own as
// printed when the sticker code is in use; otherwise one with this
// firmware's vendor and product ID.
static void svg_emit(const char *text, size_t len, void *ctx)
{
    httpd_resp_send_chunk((httpd_req_t *)ctx, text, (ssize_t)len);
}

static esp_err_t qr_get(httpd_req_t *req)
{
    touch();
    setup_code_t code;
    bool created = false;
    shelly_setup_code_current(&code, &created);
    char qr[SETUP_CODE_QR_MAX];
    if (!shelly_setup_code_qr(&code, qr)) {
        return send_result(req, "500 Internal Server Error", false, "No QR for this code");
    }
    httpd_resp_set_type(req, "image/svg+xml");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    if (!qr_svg_write(qr, svg_emit, req)) {
        return send_result(req, "500 Internal Server Error", false, "No QR for this code");
    }
    return httpd_resp_send_chunk(req, nullptr, 0);
}

static esp_err_t code_new_post(httpd_req_t *req)
{
    touch();
    shelly_setup_code_stage_random();
    return send_result(req, "200 OK", true, "");
}

// POST /switch/mode: toggle, push, follow or disabled, for SW1; "2:follow"
// and so on for SW2. It waits for 3 presses of the button, like a setup
// code (B-26).
static esp_err_t switch_mode_post(httpd_req_t *req)
{
    touch();
    char text[16];
    esp_err_t reply = ESP_OK;
    if (!read_small_body(req, text, sizeof(text), &reply)) {
        return reply;
    }
    int ch = 0;
    const char *name = text;
    if (text[0] >= '1' && text[0] <= '9' && text[1] == ':') {
        ch = text[0] - '1';
        name = text + 2;
    }
    ui_switch_mode_t mode;
    if (!ui_switch_mode_parse(name, &mode) || !shelly_switch_mode_stage_ch(ch, mode)) {
        return send_result(req, "400 Bad Request", false, "This device has no wall switch modes, or that mode is unknown");
    }
    return send_result(req, "200 OK", true, "");
}

#if CONFIG_SHELLY_DIAG_LOG
// GET /diag.txt: the diagnostics log, streamed straight from its static
// buffer (nothing writes it in this boot). Plain text, shown inline, so a
// phone's browser can display and share it.
static void diag_span(const char *bytes, size_t n, void *ctx)
{
    httpd_resp_send_chunk((httpd_req_t *)ctx, bytes, (ssize_t)n);
}

static esp_err_t diag_get(httpd_req_t *req)
{
    touch();
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    char header[160];
    size_t n = shelly_diag_log_header(header, sizeof(header));
    httpd_resp_send_chunk(req, header, (ssize_t)(n < sizeof(header) ? n : sizeof(header) - 1));
    shelly_diag_log_read(diag_span, req);
    return httpd_resp_send_chunk(req, nullptr, 0);
}

static esp_err_t diag_clear_post(httpd_req_t *req)
{
    touch();
    shelly_diag_log_clear();
    return send_result(req, "200 OK", true, "");
}
#endif

#if CONFIG_SHELLY_HW_SNAPSHOT
// GET /hw.json: the hardware snapshot of the last normal boot, and what the
// pins read now.
static esp_err_t hw_get(httpd_req_t *req)
{
    touch();
    // Slack: the live values (free heap, uptime) can grow a digit between
    // measuring and writing.
    size_t need = shelly_hw_snapshot_json(nullptr, 0) + 128;
    char *buf = (char *)malloc(need);
    if (buf == nullptr) {
        return send_result(req, "500 Internal Server Error", false, "Out of memory");
    }
    shelly_hw_snapshot_json(buf, need);
    esp_err_t err = send_json(req, "200 OK", buf);
    free(buf);
    return err;
}
#endif

#if CONFIG_SHELLY_FACTORY_DUMP
// GET /part?name=shelly: a read-only copy of one of the stock data regions
// this firmware never uses, to study what Shelly keeps there. Only reads
// flash, and only these names: never this firmware's own nvs.
static const char *const DUMP_PARTS[] = {"shelly", "fs_0", "fs_1", "zb_storage"};

static esp_err_t part_get(httpd_req_t *req)
{
    touch();
    char query[48], name[16] = "";
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
        httpd_query_key_value(query, "name", name, sizeof(name)) != ESP_OK) {
        return send_result(req, "400 Bad Request", false, "Which partition? /part?name=shelly");
    }
    bool allowed = false;
    for (const char *p : DUMP_PARTS) {
        allowed = allowed || strcmp(p, name) == 0;
    }
    const esp_partition_t *part = allowed ? esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                                                     ESP_PARTITION_SUBTYPE_ANY, name)
                                          : nullptr;
    if (part == nullptr) {
        return send_result(req, "404 Not Found", false, "Not a partition this page copies");
    }
    const size_t CHUNK = 2048;
    char *buf = (char *)malloc(CHUNK);
    if (buf == nullptr) {
        return send_result(req, "500 Internal Server Error", false, "Out of memory");
    }
    char disposition[64];
    snprintf(disposition, sizeof(disposition), "attachment; filename=\"%s-%s.bin\"", s_mac, name);
    httpd_resp_set_type(req, "application/octet-stream");
    httpd_resp_set_hdr(req, "Content-Disposition", disposition);
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    ESP_LOGI(TAG, "Copying partition %s (%lu bytes at 0x%lx) to the page", name, (unsigned long)part->size,
             (unsigned long)part->address);
    esp_err_t err = ESP_OK;
    for (size_t off = 0; off < part->size && err == ESP_OK; off += CHUNK) {
        size_t n = part->size - off < CHUNK ? part->size - off : CHUNK;
        err = esp_partition_read(part, off, buf, n);
        if (err == ESP_OK) {
            err = httpd_resp_send_chunk(req, buf, (ssize_t)n);
        }
        touch();
    }
    free(buf);
    if (err != ESP_OK) {
        // Headers are gone already: end the response short, so the download fails visibly.
        ESP_LOGW(TAG, "Copying partition %s stopped: %s", name, esp_err_to_name(err));
        return ESP_FAIL;
    }
    return httpd_resp_send_chunk(req, nullptr, 0);
}
#endif

#if CONFIG_SHELLY_HAS_METER
// A number for JSON: null for NAN.
static void json_num(char *buf, size_t len, float v, int decimals)
{
    if (!isfinite(v)) {
        snprintf(buf, len, "null");
    } else {
        snprintf(buf, len, "%.*f", decimals, (double)v);
    }
}

// GET /meter.json: the meter's live readings and the calibration in use.
static esp_err_t meter_get(httpd_req_t *req)
{
    touch();
    metering_live_t m;
    if (!metering_live(&m)) {
        return send_result(req, "404 Not Found", false, "This device has no meter");
    }
    char v[16], hz[16];
    json_num(v, sizeof(v), m.volts, 2);
    json_num(hz, sizeof(hz), m.hz, 2);
    char body[640];
    int n = snprintf(body, sizeof(body),
                     "{\"chip\":\"%s\",\"valid\":%s,\"steady\":%s,\"v\":%s,\"hz\":%s,\"raw_v\":%lld,"
                     "\"cal\":{\"v\":%.4f,\"i\":[%.4f,%.4f],\"p\":[%.4f,%.4f],\"custom\":%s},\"ch\":[",
                     m.chip, m.valid ? "true" : "false", m.steady_ready ? "true" : "false", v, hz,
                     (long long)m.raw_v, (double)m.ratios.q[CAL_Q_V], (double)m.ratios.q[CAL_Q_I0],
                     (double)m.ratios.q[CAL_Q_I1], (double)m.ratios.q[CAL_Q_P0], (double)m.ratios.q[CAL_Q_P1],
                     cal_ratios_is_default(&m.ratios) ? "false" : "true");
    for (int ch = 0; ch < m.channels && n > 0 && n < (int)sizeof(body); ch++) {
        char a[16], w[16];
        json_num(a, sizeof(a), m.amps[ch], 3);
        json_num(w, sizeof(w), m.watts[ch], 1);
        n += snprintf(body + n, sizeof(body) - n,
                      "%s{\"on\":%s,\"a\":%s,\"w\":%s,\"wh\":%.1f,\"raw_i\":%lld,\"raw_p\":%lld}",
                      ch ? "," : "", m.relay_on[ch] ? "true" : "false", a, w, (double)m.energy_mwh[ch] / 1000.0,
                      (long long)m.raw_i[ch], (long long)m.raw_p[ch]);
    }
    if (n > 0 && n < (int)sizeof(body)) {
        n += snprintf(body + n, sizeof(body) - n, "]}");
    }
    if (n < 0 || n >= (int)sizeof(body)) {
        return send_result(req, "500 Internal Server Error", false, "Readings too long");
    }
    return send_json(req, "200 OK", body);
}

// POST /cal: "ch=1&v=117.4&a=3.31&w=389", what a reference meter reads on
// the load running through that relay now. The correction is worked out
// from the device's last 5 s of readings and waits for 3 presses of the
// button, like a setup code (B-26).
static esp_err_t cal_post(httpd_req_t *req)
{
    touch();
    char text[96];
    esp_err_t reply = ESP_OK;
    if (!read_small_body(req, text, sizeof(text), &reply)) {
        return reply;
    }
    int ch = -1;
    cal_values_t ref;
    if (!cal_parse_form(text, &ch, &ref)) {
        return send_result(req, "400 Bad Request", false, cal_result_message(CAL_ERR_INPUT));
    }
    cal_values_t mean, spread;
    if (!metering_cal_measure(ch, &mean, &spread)) {
        return send_result(req, "400 Bad Request", false, cal_result_message(CAL_ERR_CHANNEL));
    }
    // Steady: each quantity being calibrated varied by at most CAL_MAX_SPREAD.
    bool stable = (isnan(ref.volts) || spread.volts <= CAL_MAX_SPREAD) &&
                  (isnan(ref.amps) || spread.amps <= CAL_MAX_SPREAD) &&
                  (isnan(ref.watts) || spread.watts <= CAL_MAX_SPREAD);
    cal_ratios_t cur, out;
    shelly_cal_get(&cur);
    cal_result_t r = cal_compute(&cur, RELAY_CHANNELS, ch, &mean, stable, &ref, &out);
    if (r != CAL_OK) {
        return send_result(req, "400 Bad Request", false, cal_result_message(r));
    }
    if (!shelly_cal_stage(ch, &out)) {
        return send_result(req, "400 Bad Request", false, cal_result_message(CAL_ERR_RANGE));
    }
    ESP_LOGI(TAG, "Calibration staged for relay %d: V x%.4f, I x%.4f, P x%.4f", ch + 1, out.q[CAL_Q_V],
             out.q[CAL_Q_I(ch)], out.q[CAL_Q_P(ch)]);
    return send_result(req, "200 OK", true, "");
}

// POST /cal/reset: back to the built-in calibration, after 3 presses.
static esp_err_t cal_reset_post(httpd_req_t *req)
{
    touch();
    cal_ratios_t def;
    cal_ratios_default(&def);
    if (!shelly_cal_stage(0, &def)) {
        return send_result(req, "400 Bad Request", false, "This device has no meter");
    }
    return send_result(req, "200 OK", true, "");
}
#endif

static esp_err_t change_cancel_post(httpd_req_t *req)
{
    touch();
    shelly_change_cancel();
    return send_result(req, "200 OK", true, "");
}

static esp_err_t upload_fail(httpd_req_t *req, const char *status, const char *msg)
{
    ESP_LOGW(TAG, "Upload refused: %s", msg);
    s_uploading = false;
    status_led_set_state(LED_STATE_MAINTENANCE);
    status_led_flash_refused();
    touch();
    return send_result(req, status, false, msg);
}

static esp_err_t update_post(httpd_req_t *req)
{
    if (s_restart) {
        // An update was just installed and set to boot: a second upload
        // would rewrite that slot and be cut off by the restart.
        return send_result(req, "503 Service Unavailable", false, "The device is restarting");
    }
    touch();
    char query[32] = "";
    bool allow_older = httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK &&
                       strstr(query, "allow_older=1") != nullptr;

    size_t total = req->content_len;
    if (total == 0 || total > MAX_UPLOAD_BYTES) {
        return upload_fail(req, "400 Bad Request", "The file is empty or too large for this device");
    }

    s_uploading = true;
    status_led_set_state(LED_STATE_UPLOADING);

    // Read just enough to check the file before erasing anything.
    const size_t prefix_cap = MI_OTA_FIXED_HEADER + MI_OTA_MAX_TLV + MI_HW_DESC_OFFSET + sizeof(shelly_hw_desc_t);
    uint8_t *buf = (uint8_t *)malloc(prefix_cap > UPLOAD_CHUNK ? prefix_cap : UPLOAD_CHUNK);
    if (buf == nullptr) {
        return upload_fail(req, "500 Internal Server Error", "Out of memory");
    }
    size_t have = 0;
    mi_info_t info;
    mi_result_t r;
    while ((r = mi_check(buf, have, shelly_hw_desc.hw_code, &info)) == MI_NEED_MORE) {
        size_t want = info.need_bytes > prefix_cap ? prefix_cap : info.need_bytes;
        if (want <= have || have >= total) {
            break;
        }
        if (want > total) {
            want = total;
        }
        int n = recv_some(req, buf + have, want - have);
        if (n <= 0) {
            free(buf);
            s_uploading = false;
            status_led_set_state(LED_STATE_MAINTENANCE);
            return ESP_FAIL;  // The connection is gone; nothing was written
        }
        have += (size_t)n;
    }
    if (r != MI_OK) {
        free(buf);
        return upload_fail(req, "400 Bad Request", mi_result_text(r));
    }
    if (info.total_size != total) {
        free(buf);
        return upload_fail(req, "400 Bad Request", "The file is incomplete or damaged");
    }

    const esp_app_desc_t *running = esp_app_get_description();
    // Versions only order the builds of one variant: each variant has its
    // own version line, so a variant change is never "older".
    bool variant_change = strcmp(info.project, running->project_name) != 0;
    if (!variant_change && mi_version_cmp(info.version, running->version) < 0 && !allow_older) {
        free(buf);
        return upload_fail(req, "409 Conflict",
                           "That firmware is older than the one installed. Tick Allow installing an older version");
    }

    const esp_partition_t *part = esp_ota_get_next_update_partition(nullptr);
    if (part != nullptr && info.payload_size > part->size) {
        free(buf);
        return upload_fail(req, "400 Bad Request", "The firmware is too large for this device");
    }
    esp_ota_handle_t ota = 0;
    esp_err_t err = part == nullptr ? ESP_ERR_NOT_FOUND : esp_ota_begin(part, OTA_WITH_SEQUENTIAL_WRITES, &ota);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin: %s", esp_err_to_name(err));
        free(buf);
        return upload_fail(req, "500 Internal Server Error", "The device could not start the update");
    }
    ESP_LOGI(TAG, "Installing %s %s (%u bytes) into %s", info.project, info.version, (unsigned)info.payload_size,
             part->label);

    size_t received = have;
    bool connection_lost = false;
    err = esp_ota_write(ota, buf + info.payload_offset, have - info.payload_offset);
    while (err == ESP_OK && received < total) {
        size_t want = total - received > UPLOAD_CHUNK ? UPLOAD_CHUNK : total - received;
        int n = recv_some(req, buf, want);
        if (n <= 0) {
            connection_lost = true;
            err = ESP_FAIL;
            break;
        }
        received += (size_t)n;
        touch();
        err = esp_ota_write(ota, buf, (size_t)n);
    }
    free(buf);
    if (err != ESP_OK) {
        esp_ota_abort(ota);
        ESP_LOGE(TAG, "Upload stopped after %u of %u bytes: %s", (unsigned)received, (unsigned)total,
                 esp_err_to_name(err));
        if (connection_lost) {
            s_uploading = false;
            status_led_set_state(LED_STATE_MAINTENANCE);
            return ESP_FAIL;  // The connection is gone: nobody to answer
        }
        // A flash write failed: say so, whatever is still in flight.
        return upload_fail(req, "500 Internal Server Error", "Writing the firmware failed. Nothing was installed");
    }

    // esp_ota_end checks the image's checksum and SHA-256.
    err = esp_ota_end(ota);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_end: %s", esp_err_to_name(err));
        return upload_fail(req, "400 Bad Request", "The firmware failed its integrity check. Nothing was installed");
    }
    err = esp_ota_set_boot_partition(part);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_set_boot_partition: %s", esp_err_to_name(err));
        return upload_fail(req, "500 Internal Server Error", "The firmware could not be activated");
    }

    ESP_LOGW(TAG, "Installed %s %s; restarting into it", info.project, info.version);
    char project[sizeof(info.project)];
    char version[sizeof(info.version)];
    json_safe(project, sizeof(project), info.project);
    json_safe(version, sizeof(version), info.version);
    char msg[240];
    snprintf(msg, sizeof(msg),
             variant_change ? "Installed the %s variant, version %s. The device restarts now. It is a different "
                              "device type: remove it from your controller and pair it again"
                            : "Installed %s version %s. The device restarts now and confirms the update after a minute",
             project, version);
    send_result(req, "200 OK", true, msg);
    s_restart = true;
    return ESP_OK;
}

static esp_err_t start_http(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.stack_size = HTTP_STACK_SIZE;
    cfg.max_open_sockets = 4;
    cfg.lru_purge_enable = true;
    cfg.max_uri_handlers = 18;
    cfg.recv_wait_timeout = 10;
    cfg.send_wait_timeout = 10;

    httpd_handle_t server = nullptr;
    esp_err_t err = httpd_start(&server, &cfg);
    if (err != ESP_OK) {
        return err;
    }
    static const httpd_uri_t uris[] = {
        {.uri = "/", .method = HTTP_GET, .handler = page_get, .user_ctx = nullptr},
        {.uri = "/status", .method = HTTP_GET, .handler = status_get, .user_ctx = nullptr},
        {.uri = "/toggle", .method = HTTP_POST, .handler = toggle_post, .user_ctx = nullptr},
        {.uri = "/exit", .method = HTTP_POST, .handler = exit_post, .user_ctx = nullptr},
        {.uri = "/update", .method = HTTP_POST, .handler = update_post, .user_ctx = nullptr},
        {.uri = "/code/set", .method = HTTP_POST, .handler = code_set_post, .user_ctx = nullptr},
        {.uri = "/code/new", .method = HTTP_POST, .handler = code_new_post, .user_ctx = nullptr},
        {.uri = "/qr.svg", .method = HTTP_GET, .handler = qr_get, .user_ctx = nullptr},
        {.uri = "/switch/mode", .method = HTTP_POST, .handler = switch_mode_post, .user_ctx = nullptr},
        {.uri = "/change/cancel", .method = HTTP_POST, .handler = change_cancel_post, .user_ctx = nullptr},
#if CONFIG_SHELLY_DIAG_LOG
        {.uri = "/diag.txt", .method = HTTP_GET, .handler = diag_get, .user_ctx = nullptr},
        {.uri = "/diag/clear", .method = HTTP_POST, .handler = diag_clear_post, .user_ctx = nullptr},
#endif
#if CONFIG_SHELLY_HW_SNAPSHOT
        {.uri = "/hw.json", .method = HTTP_GET, .handler = hw_get, .user_ctx = nullptr},
#endif
#if CONFIG_SHELLY_HAS_METER
        {.uri = "/meter.json", .method = HTTP_GET, .handler = meter_get, .user_ctx = nullptr},
        {.uri = "/cal", .method = HTTP_POST, .handler = cal_post, .user_ctx = nullptr},
        {.uri = "/cal/reset", .method = HTTP_POST, .handler = cal_reset_post, .user_ctx = nullptr},
#endif
#if CONFIG_SHELLY_FACTORY_DUMP
        {.uri = "/part", .method = HTTP_GET, .handler = part_get, .user_ctx = nullptr},
#endif
    };
    static_assert(sizeof(uris) / sizeof(uris[0]) <= 18, "raise max_uri_handlers");
    for (const httpd_uri_t &uri : uris) {
        err = httpd_register_uri_handler(server, &uri);
        if (err != ESP_OK) {
            return err;
        }
    }
    return httpd_register_err_handler(server, HTTPD_404_NOT_FOUND, redirect_404);
}

// ---------------------------------------------------------------------------
// Run
// ---------------------------------------------------------------------------

[[noreturn]] static void run(uint32_t relays_on);

void shelly_maintenance_run(bool relay_was_on, shelly_ui_toggle_fn_t local_toggle)
{
    s_local_toggle = local_toggle;
    run(relay_was_on ? 1u : 0u);
}

void shelly_maintenance_run_ch(uint32_t relays_on, bool (*local_toggle_ch)(int ch))
{
    s_local_toggle_ch = local_toggle_ch;
    run(relays_on);
}

static void run(uint32_t relays_on)
{
    ESP_LOGW(TAG, "Wi-Fi maintenance mode: Matter and Thread are off for this boot");
    status_led_set_state(LED_STATE_MAINTENANCE);
    touch();

    for (int ch = 0; ch < RELAY_CHANNELS; ch++) {
        if ((relays_on & (1u << ch)) != 0 && relay_set_ch(ch, true) != ESP_OK) {
            ESP_LOGW(TAG, "Relay %d not restored: turn-on refused", ch + 1);
        }
    }

    esp_err_t err = start_wifi();
    if (err == ESP_OK) {
        err = start_http();
    }
    if (err != ESP_OK) {
        // Fail safe: back to normal operation rather than a device that
        // neither runs Matter nor offers its page.
        ESP_LOGE(TAG, "Maintenance mode failed to start (%s); restarting normally", esp_err_to_name(err));
        status_led_flash_refused();
        vTaskDelay(pdMS_TO_TICKS(2000));
        // Not a clean exit: if the meter took the memory Wi-Fi needed, the
        // next attempt runs without it (shelly_maintenance_meter_allowed()).
        shelly_maintenance_failed_record();
        esp_restart();
    }
    if (xTaskCreate(dns_task, "maint_dns", DNS_STACK_SIZE, nullptr, 4, nullptr) != pdPASS) {
        // The page still works at 192.168.33.1; only the automatic popup is lost.
        ESP_LOGW(TAG, "No captive-portal DNS");
    }

    // This boot has no health task: watch this loop instead, so a wedged
    // maintenance boot restarts (normally, the request was consumed).
    bool wdt = esp_task_wdt_add(nullptr) == ESP_OK;

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(500));
        if (wdt) {
            esp_task_wdt_reset();
        }
        if (s_restart) {
            vTaskDelay(pdMS_TO_TICKS(RESTART_DELAY_MS));
            restart_to_normal();
        }
        if (!s_uploading && uptime_ms() - s_last_activity_ms > IDLE_TIMEOUT_MS) {
            ESP_LOGI(TAG, "No activity for %d s: back to normal operation", CONFIG_SHELLY_MAINTENANCE_IDLE_TIMEOUT_S);
            restart_to_normal();
        }
    }
}
