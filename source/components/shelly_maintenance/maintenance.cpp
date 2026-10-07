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

#include <string.h>

#include <esp_app_desc.h>
#include <esp_event.h>
#include <esp_http_server.h>
#include <esp_log.h>
#include <esp_mac.h>
#include <esp_netif.h>
#include <esp_ota_ops.h>
#include <esp_system.h>
#include <esp_task_wdt.h>
#include <esp_timer.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <dhcpserver/dhcpserver.h>
#include <lwip/sockets.h>
#include <sdkconfig.h>

#include <relay.h>
#include <shelly_hw_desc.h>
#include <status_led.h>

static const char *TAG = "maintenance";

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

// Every way out of the mode into normal operation: the normal boot keeps
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

    wifi_config_t ap_cfg = {};
    int n = snprintf((char *)ap_cfg.ap.ssid, sizeof(ap_cfg.ap.ssid), "Shelly-%s-%02X%02X%02X",
                     shelly_hw_desc.hw_code, mac[3], mac[4], mac[5]);
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
    char body[256];
    snprintf(body, sizeof(body),
             "{\"hw\":\"%s\",\"variant\":\"%s\",\"version\":\"%s\",\"mac\":\"%s\",\"relay\":%s}",
             shelly_hw_desc.hw_code, project, version, s_mac, relay_get() ? "true" : "false");
    return send_json(req, "200 OK", body);
}

static esp_err_t toggle_post(httpd_req_t *req)
{
    touch();
    bool ok = s_local_toggle != nullptr && s_local_toggle();
    if (!ok) {
        status_led_flash_refused();
    }
    return send_result(req, "200 OK", ok, ok ? "" : "The relay refused to switch (over-temperature or protection)");
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
    };
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

void shelly_maintenance_run(bool relay_was_on, shelly_ui_toggle_fn_t local_toggle)
{
    ESP_LOGW(TAG, "Wi-Fi maintenance mode: Matter and Thread are off for this boot");
    s_local_toggle = local_toggle;
    status_led_set_state(LED_STATE_MAINTENANCE);
    touch();

    if (relay_was_on && relay_set(true) != ESP_OK) {
        ESP_LOGW(TAG, "Relay not restored: turn-on refused");
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
        restart_to_normal();
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
