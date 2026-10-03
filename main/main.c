// wyze-esp-bridge: relays a Wyze Sense Bridge dongle (USB 1a86:e024) to a
// wyzesense2mqtt-rs gateway over WebSocket, like the gateway's dongle_bridge.
//
// Boards (selected by IDF target):
//   esp32p4: Waveshare ESP32-P4-WIFI6-POE-ETH. Internal EMAC + IP101 PHY; dongle on the USB-A
//            port (USB 2.0 HS controller); console on UART0 via the USB-C port's CH343.
//   esp32s3: Waveshare ESP32-S3-ETH. W5500 SPI Ethernet; dongle on the USB-C port through an OTG
//            adapter; console on UART0 header pins GPIO43 TX / GPIO44 RX.
//
// Dongle -> gateway: each 64-byte HID input report is [len][len protocol bytes][stale];
//   only the protocol bytes are forwarded, one binary frame per report.
// Gateway -> dongle: each binary frame is one protocol packet, written as a HID
//   SET_REPORT (Output, report ID 0). The dongle has no interrupt OUT endpoint.
//
// Gateway settings live in NVS and are set from the console. An HTTP server on port 80
// serves /status and /log, and takes firmware updates on POST /ota.

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "driver/uart.h"
#include "esp_app_desc.h"
#include "esp_check.h"
#include "esp_eth.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_websocket_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "usb/hid_host.h"
#include "usb/usb_host.h"

static const char *TAG = "wyze-bridge";

#define DONGLE_VID 0x1a86
#define DONGLE_PID 0xe024
#define HID_REPORT_LEN 64
#define MAX_FRAME 128
#define CONSOLE_UART UART_NUM_0
#define HOSTNAME "wyze-esp-bridge"

#if CONFIG_IDF_TARGET_ESP32P4
#define BOARD_NAME "esp32p4-eth"
// IP101 PHY reset on the Waveshare ESP32-P4-WIFI6-POE-ETH. The RMII and SMI pins
// (MDC 31, MDIO 52, 50 MHz clock in on 50) match ETH_ESP32_EMAC_DEFAULT_CONFIG().
#define ETH_PIN_PHY_RST 51
#else
#define BOARD_NAME "esp32s3-eth"
// W5500 wiring on the Waveshare ESP32-S3-ETH
#define ETH_SPI_HOST SPI2_HOST
#define ETH_PIN_MOSI 11
#define ETH_PIN_MISO 12
#define ETH_PIN_SCLK 13
#define ETH_PIN_CS 14
#define ETH_PIN_INT 10
#define ETH_PIN_RST 9
#define ETH_SPI_MHZ 20
#endif

// ---------------------------------------------------------------- log ring buffer

#define LOG_BUF_SIZE 16384

static char s_log[LOG_BUF_SIZE];
static size_t s_log_head;  // next write position
static bool s_log_wrapped;
static portMUX_TYPE s_log_lock = portMUX_INITIALIZER_UNLOCKED;

static int log_vprintf(const char *fmt, va_list args)
{
    char line[256];
    va_list copy;
    va_copy(copy, args);
    int n = vsnprintf(line, sizeof(line), fmt, copy);
    va_end(copy);
    if (n > 0) {
        size_t len = n < (int)sizeof(line) ? (size_t)n : sizeof(line) - 1;
        taskENTER_CRITICAL(&s_log_lock);
        for (size_t i = 0; i < len; i++) {
            s_log[s_log_head++] = line[i];
            if (s_log_head == LOG_BUF_SIZE) {
                s_log_head = 0;
                s_log_wrapped = true;
            }
        }
        taskEXIT_CRITICAL(&s_log_lock);
    }
    return vprintf(fmt, args);
}

// ---------------------------------------------------------------- config

typedef struct {
    char uri[128];
    char token[65];
} bridge_config_t;

static bridge_config_t s_cfg;

static void nvs_get_str_or(nvs_handle_t h, const char *key, char *out, size_t size, const char *fallback)
{
    size_t len = size;
    if (nvs_get_str(h, key, out, &len) != ESP_OK) {
        strlcpy(out, fallback, size);
    }
}

static void config_load(void)
{
    nvs_handle_t h;
    if (nvs_open("bridge", NVS_READONLY, &h) != ESP_OK) {
        strlcpy(s_cfg.uri, CONFIG_WYZE_GATEWAY_URI, sizeof(s_cfg.uri));
        strlcpy(s_cfg.token, CONFIG_WYZE_BRIDGE_TOKEN, sizeof(s_cfg.token));
        return;
    }
    nvs_get_str_or(h, "uri", s_cfg.uri, sizeof(s_cfg.uri), CONFIG_WYZE_GATEWAY_URI);
    nvs_get_str_or(h, "token", s_cfg.token, sizeof(s_cfg.token), CONFIG_WYZE_BRIDGE_TOKEN);
    nvs_close(h);
}

static esp_err_t config_save(void)
{
    nvs_handle_t h;
    ESP_RETURN_ON_ERROR(nvs_open("bridge", NVS_READWRITE, &h), TAG, "nvs_open");
    nvs_set_str(h, "uri", s_cfg.uri);
    nvs_set_str(h, "token", s_cfg.token);
    esp_err_t err = nvs_commit(h);
    nvs_close(h);
    return err;
}

// Reads a line from the console UART. Returns false on timeout before any *printable* input.
// Non-printable bytes (0x00 etc. from USB-UART line toggles when a terminal attaches
// mid-boot) must not cancel the setup-prompt timeout: they used to switch the read to
// an infinite wait, hanging boot until a terminal pressed Enter.
static bool console_read_line(char *out, size_t size, bool secret, TickType_t first_key_timeout)
{
    size_t n = 0;
    TickType_t timeout = first_key_timeout;
    while (true) {
        uint8_t c;
        if (uart_read_bytes(CONSOLE_UART, &c, 1, timeout) != 1) {
            if (n == 0 && timeout != portMAX_DELAY) {
                return false;
            }
            continue;
        }
        if (n == 0 && timeout != portMAX_DELAY && !(c >= 0x20 && c < 0x7f) && c != '\r' && c != '\n') {
            continue;  // ignore line-noise before input starts; keep the deadline
        }
        timeout = portMAX_DELAY;
        if (c == '\r' || c == '\n') {
            if (n == 0 && c == '\n') {
                continue;  // swallow the LF of a CRLF pair
            }
            break;
        }
        if ((c == 0x08 || c == 0x7f) && n > 0) {
            n--;
            uart_write_bytes(CONSOLE_UART, "\b \b", 3);
            continue;
        }
        if (c >= 0x20 && c < 0x7f && n + 1 < size) {
            out[n++] = (char)c;
            uart_write_bytes(CONSOLE_UART, secret ? "*" : (const char *)&c, 1);
        }
    }
    out[n] = '\0';
    uart_write_bytes(CONSOLE_UART, "\r\n", 2);
    return true;
}

static void prompt(const char *label, char *field, size_t size, bool secret)
{
    char line[128];
    const char *shown = secret ? (field[0] ? "<unchanged>" : "") : field;
    printf("%s [%s]: ", label, shown);
    fflush(stdout);
    console_read_line(line, sizeof(line), secret, portMAX_DELAY);
    if (line[0]) {
        strlcpy(field, line, size);
    }
}

// Built-in defaults work as-is, so setup is only offered, never required.
static void console_setup(void)
{
    ESP_ERROR_CHECK(uart_driver_install(CONSOLE_UART, 256, 0, 0, NULL, 0));
    printf("\nGateway %s\nPress Enter within 3 s to change settings...\n", s_cfg.uri);
    char line[8];
    if (!console_read_line(line, sizeof(line), false, pdMS_TO_TICKS(3000))) {
        return;
    }
    printf("\n=== wyze-esp-bridge setup (Enter keeps the value in brackets) ===\n");
    prompt("Gateway URI", s_cfg.uri, sizeof(s_cfg.uri), false);
    prompt("Bridge token", s_cfg.token, sizeof(s_cfg.token), true);
    ESP_ERROR_CHECK(config_save());
    printf("Saved. Restarting...\n");
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
}

// ---------------------------------------------------------------- state

typedef struct {
    uint16_t len;
    uint8_t data[MAX_FRAME];
} frame_t;

typedef struct {
    hid_host_device_handle_t handle;
    hid_host_driver_event_t event;
} hid_driver_msg_t;

#define NET_UP_BIT BIT0

static EventGroupHandle_t s_events;
static QueueHandle_t s_hid_driver_q;  // new HID devices (driver callback -> app task)
static QueueHandle_t s_to_gateway_q;  // dongle -> gateway frames
static QueueHandle_t s_to_dongle_q;   // gateway -> dongle frames
static volatile hid_host_device_handle_t s_dongle;
static volatile bool s_dongle_gone;
static esp_websocket_client_handle_t s_ws;
static esp_netif_t *s_netif;
static volatile uint32_t s_frames_up, s_frames_down;

// ---------------------------------------------------------------- Ethernet

static void net_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == ETH_EVENT && id == ETHERNET_EVENT_CONNECTED) {
        ESP_LOGI(TAG, "ethernet link up");
    } else if (base == ETH_EVENT && id == ETHERNET_EVENT_DISCONNECTED) {
        ESP_LOGW(TAG, "ethernet link down");
        xEventGroupClearBits(s_events, NET_UP_BIT);
    } else if (base == IP_EVENT && id == IP_EVENT_ETH_GOT_IP) {
        ip_event_got_ip_t *ev = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "network up, IP " IPSTR, IP2STR(&ev->ip_info.ip));
        xEventGroupSetBits(s_events, NET_UP_BIT);
        // A new image that gets on the network is kept; otherwise the bootloader rolls back.
        esp_ota_mark_app_valid_cancel_rollback();
    }
}

static void eth_start(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    esp_netif_config_t netif_cfg = ESP_NETIF_DEFAULT_ETH();
    s_netif = esp_netif_new(&netif_cfg);
    esp_netif_set_hostname(s_netif, HOSTNAME);

    eth_mac_config_t mac_cfg = ETH_MAC_DEFAULT_CONFIG();
    eth_phy_config_t phy_cfg = ETH_PHY_DEFAULT_CONFIG();
#if CONFIG_IDF_TARGET_ESP32P4
    eth_esp32_emac_config_t emac = ETH_ESP32_EMAC_DEFAULT_CONFIG();
    phy_cfg.phy_addr = ESP_ETH_PHY_ADDR_AUTO;
    phy_cfg.reset_gpio_num = ETH_PIN_PHY_RST;
    esp_eth_mac_t *mac = esp_eth_mac_new_esp32(&emac, &mac_cfg);
    esp_eth_phy_t *phy = esp_eth_phy_new_ip101(&phy_cfg);
#else
    ESP_ERROR_CHECK(gpio_install_isr_service(0));
    const spi_bus_config_t bus = {
        .miso_io_num = ETH_PIN_MISO,
        .mosi_io_num = ETH_PIN_MOSI,
        .sclk_io_num = ETH_PIN_SCLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(ETH_SPI_HOST, &bus, SPI_DMA_CH_AUTO));
    spi_device_interface_config_t dev = {
        .mode = 0,
        .clock_speed_hz = ETH_SPI_MHZ * 1000 * 1000,
        .queue_size = 20,
        .spics_io_num = ETH_PIN_CS,
    };
    eth_w5500_config_t w5500 = ETH_W5500_DEFAULT_CONFIG(ETH_SPI_HOST, &dev);
    w5500.int_gpio_num = ETH_PIN_INT;
    phy_cfg.reset_gpio_num = ETH_PIN_RST;
    esp_eth_mac_t *mac = esp_eth_mac_new_w5500(&w5500, &mac_cfg);
    esp_eth_phy_t *phy = esp_eth_phy_new_w5500(&phy_cfg);
#endif
    esp_eth_config_t eth_cfg = ETH_DEFAULT_CONFIG(mac, phy);
    esp_eth_handle_t eth = NULL;
    ESP_ERROR_CHECK(esp_eth_driver_install(&eth_cfg, &eth));

    // Use the chip's factory Ethernet MAC (the W5500 has none of its own).
    uint8_t addr[6];
    ESP_ERROR_CHECK(esp_read_mac(addr, ESP_MAC_ETH));
    ESP_ERROR_CHECK(esp_eth_ioctl(eth, ETH_CMD_S_MAC_ADDR, addr));
    ESP_LOGI(TAG, "ethernet MAC %02x:%02x:%02x:%02x:%02x:%02x", addr[0], addr[1], addr[2], addr[3], addr[4],
             addr[5]);

    ESP_ERROR_CHECK(esp_netif_attach(s_netif, esp_eth_new_netif_glue(eth)));
    ESP_ERROR_CHECK(esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID, net_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, net_event_handler, NULL));
    ESP_ERROR_CHECK(esp_eth_start(eth));
}

// ---------------------------------------------------------------- USB / HID

// If the dongle still isn't up this long after boot, bounce root port power
// once more (pre-enumeration only; skipped once the dongle is running).
static void usb_wdg_task(void *arg)
{
    for (int i = 0; i < 4; i++) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        if (s_dongle) {
            break;
        }
        if (usb_host_lib_set_root_port_power(false) == ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(200));
            if (usb_host_lib_set_root_port_power(true) == ESP_OK) {
                ESP_LOGI(TAG, "root port power bounced (dongle not up)");
            }
        }
    }
    vTaskDelete(NULL);
}

static void hid_interface_cb(hid_host_device_handle_t handle, const hid_host_interface_event_t event, void *arg)
{
    switch (event) {
    case HID_HOST_INTERFACE_EVENT_INPUT_REPORT: {
        uint8_t raw[HID_REPORT_LEN];
        size_t n = 0;
        if (hid_host_device_get_raw_input_report_data(handle, raw, sizeof(raw), &n) != ESP_OK || n == 0) {
            return;
        }
        frame_t f;
        if (raw[0] == 0x55 || raw[0] == 0xAA) {
            // No length prefix: the whole report is protocol data.
            f.len = n;
            memcpy(f.data, raw, n);
        } else {
            size_t len = raw[0] > 0x3F ? 0x3F : raw[0];
            if (len + 1 > n) {
                len = n - 1;
            }
            if (len == 0) {
                return;
            }
            f.len = len;
            memcpy(f.data, raw + 1, len);
        }
        if (xQueueSend(s_to_gateway_q, &f, 0) != pdTRUE) {
            ESP_LOGW(TAG, "gateway queue full, dropped %u bytes", f.len);
        }
        break;
    }
    case HID_HOST_INTERFACE_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "Wyze dongle disconnected");
        if (handle == s_dongle) {
            s_dongle = NULL;
            s_dongle_gone = true;
        }
        hid_host_device_close(handle);
        break;
    case HID_HOST_INTERFACE_EVENT_TRANSFER_ERROR:
        ESP_LOGW(TAG, "HID transfer error");
        break;
    default:
        break;
    }
}

static void hid_driver_cb(hid_host_device_handle_t handle, const hid_host_driver_event_t event, void *arg)
{
    // Runs in the HID driver task: hand off to the app task.
    hid_driver_msg_t msg = {.handle = handle, .event = event};
    xQueueSend(s_hid_driver_q, &msg, 0);
}

static void usb_lib_task(void *arg)
{
    const usb_host_config_t host_config = {
        .intr_flags = ESP_INTR_FLAG_LEVEL1,
#if CONFIG_IDF_TARGET_ESP32P4
        // The CH9350 in the pairing dongle mis-enumerates when it was already
        // powered during the host's boot-time root port activity ("Root port
        // reset failed" spam -> hub.c abort). A replug always heals it, so
        // keep the root port unpowered through install and emulate the replug
        // here: a fixed settle window with port power off, then power on
        // before any hub events have been processed.
        .root_port_unpowered = true,
#endif
    };
    ESP_ERROR_CHECK(usb_host_install(&host_config));
    xTaskNotifyGive((TaskHandle_t)arg);
#if CONFIG_IDF_TARGET_ESP32P4
    ESP_LOGI(TAG, "root port power off for settle");
    vTaskDelay(pdMS_TO_TICKS(1000));
    ESP_ERROR_CHECK(usb_host_lib_set_root_port_power(true));
    ESP_LOGI(TAG, "root port power on");
    xTaskCreate(usb_wdg_task, "usbwd", 3072, NULL, 4, NULL);
#endif
    while (true) {
        uint32_t flags;
        usb_host_lib_handle_events(portMAX_DELAY, &flags);
        if (flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) {
            usb_host_device_free_all();
        }
    }
}

static void open_hid_device(hid_host_device_handle_t handle)
{
    hid_host_dev_info_t info;
    if (hid_host_get_device_info(handle, &info) != ESP_OK) {
        ESP_LOGW(TAG, "could not read HID device info");
        return;
    }
    if (info.VID != DONGLE_VID || info.PID != DONGLE_PID) {
        ESP_LOGI(TAG, "ignoring HID device %04x:%04x", info.VID, info.PID);
        return;
    }
    const hid_host_device_config_t dev_config = {.callback = hid_interface_cb, .callback_arg = NULL};
    if (hid_host_device_open(handle, &dev_config) != ESP_OK || hid_host_device_start(handle) != ESP_OK) {
        ESP_LOGE(TAG, "failed to open Wyze dongle");
        return;
    }
    s_dongle_gone = false;
    s_dongle = handle;
    ESP_LOGI(TAG, "Wyze dongle up, connecting to gateway");
}

static void to_dongle_task(void *arg)
{
    frame_t f;
    while (true) {
        if (xQueueReceive(s_to_dongle_q, &f, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        hid_host_device_handle_t dev = s_dongle;
        if (!dev) {
            continue;
        }
        // Mirrors a Linux hidraw write on a device without an OUT endpoint.
        esp_err_t err = hid_class_request_set_report(dev, HID_REPORT_TYPE_OUTPUT, 0, f.data, f.len);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "SET_REPORT (%u bytes) failed: %s", f.len, esp_err_to_name(err));
        } else {
            s_frames_down++;
        }
    }
}

// ---------------------------------------------------------------- WebSocket

static frame_t s_rx;  // reassembly buffer for fragmented frames (WebSocket task only)

static void ws_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    esp_websocket_event_data_t *ev = (esp_websocket_event_data_t *)data;
    switch (id) {
    case WEBSOCKET_EVENT_CONNECTED:
        ESP_LOGI(TAG, "gateway connected");
        xQueueReset(s_to_dongle_q);
        break;
    case WEBSOCKET_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "gateway disconnected");
        break;
    case WEBSOCKET_EVENT_DATA:
        if (ev->op_code != 0x02 && !(ev->op_code == 0x00 && s_rx.len > 0)) {
            return;  // only binary frames carry protocol data
        }
        if (ev->payload_len > MAX_FRAME || ev->payload_offset + ev->data_len > MAX_FRAME) {
            ESP_LOGW(TAG, "frame too large (%d bytes), dropped", ev->payload_len);
            s_rx.len = 0;
            return;
        }
        memcpy(s_rx.data + ev->payload_offset, ev->data_ptr, ev->data_len);
        s_rx.len = ev->payload_offset + ev->data_len;
        if (s_rx.len >= ev->payload_len) {
            if (xQueueSend(s_to_dongle_q, &s_rx, pdMS_TO_TICKS(100)) != pdTRUE) {
                ESP_LOGW(TAG, "dongle queue full, dropped frame");
            }
            s_rx.len = 0;
        }
        break;
    case WEBSOCKET_EVENT_ERROR:
        ESP_LOGW(TAG, "gateway connection error");
        break;
    default:
        break;
    }
}

static void ws_start(void)
{
    static char uri[256];
    const char *sep = strchr(s_cfg.uri, '?') ? "&" : "?";
    if (s_cfg.token[0]) {
        snprintf(uri, sizeof(uri), "%s%sdevice=" BOARD_NAME "&token=%s", s_cfg.uri, sep, s_cfg.token);
    } else {
        snprintf(uri, sizeof(uri), "%s%sdevice=" BOARD_NAME, s_cfg.uri, sep);
    }
    const esp_websocket_client_config_t cfg = {
        .uri = uri,
        .reconnect_timeout_ms = 5000,
        .network_timeout_ms = 10000,
        .buffer_size = 1024,
    };
    s_rx.len = 0;
    s_ws = esp_websocket_client_init(&cfg);
    esp_websocket_register_events(s_ws, WEBSOCKET_EVENT_ANY, ws_event_handler, NULL);
    esp_websocket_client_start(s_ws);
    ESP_LOGI(TAG, "connecting to %s", s_cfg.uri);
}

static void ws_stop(void)
{
    if (!s_ws) {
        return;
    }
    esp_websocket_client_stop(s_ws);
    esp_websocket_client_destroy(s_ws);
    s_ws = NULL;
    ESP_LOGI(TAG, "gateway connection closed");
}

static void to_gateway_task(void *arg)
{
    frame_t f;
    while (true) {
        if (xQueueReceive(s_to_gateway_q, &f, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        esp_websocket_client_handle_t ws = s_ws;
        if (ws && esp_websocket_client_is_connected(ws)) {
            if (esp_websocket_client_send_bin(ws, (const char *)f.data, f.len, pdMS_TO_TICKS(1000)) < 0) {
                ESP_LOGW(TAG, "send to gateway failed");
            } else {
                s_frames_up++;
            }
        }
    }
}

// ---------------------------------------------------------------- HTTP: status, log, OTA

// Accepts the bridge token as "Authorization: Bearer <token>" or ?token=<token>.
static bool http_authorized(httpd_req_t *req)
{
    if (!s_cfg.token[0]) {
        return true;
    }
    char buf[96];
    if (httpd_req_get_hdr_value_str(req, "Authorization", buf, sizeof(buf)) == ESP_OK &&
        strncmp(buf, "Bearer ", 7) == 0 && strcmp(buf + 7, s_cfg.token) == 0) {
        return true;
    }
    char query[128];
    char token[72];
    return httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK &&
           httpd_query_key_value(query, "token", token, sizeof(token)) == ESP_OK && strcmp(token, s_cfg.token) == 0;
}

static esp_err_t status_get(httpd_req_t *req)
{
    const esp_app_desc_t *app = esp_app_get_description();
    esp_netif_ip_info_t ip = {0};
    esp_netif_get_ip_info(s_netif, &ip);
    esp_websocket_client_handle_t ws = s_ws;
    char body[640];
    snprintf(body, sizeof(body),
             "{\"board\":\"" BOARD_NAME "\",\"version\":\"%s\",\"built\":\"%s %s\",\"uptime_s\":%lld,\"reset_reason\":%d,"
             "\"ip\":\"" IPSTR "\",\"gateway\":\"%s\",\"dongle\":%s,\"gateway_connected\":%s,"
             "\"frames_to_gateway\":%lu,\"frames_to_dongle\":%lu,\"free_heap\":%lu,\"partition\":\"%s\"}\n",
             app->version, app->date, app->time, esp_timer_get_time() / 1000000, esp_reset_reason(),
             IP2STR(&ip.ip), s_cfg.uri, s_dongle ? "true" : "false",
             (ws && esp_websocket_client_is_connected(ws)) ? "true" : "false", (unsigned long)s_frames_up,
             (unsigned long)s_frames_down, (unsigned long)esp_get_free_heap_size(),
             esp_ota_get_running_partition()->label);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, body);
}

static esp_err_t log_get(httpd_req_t *req)
{
    // Copy out under the lock, then send without holding it.
    char *copy = malloc(LOG_BUF_SIZE);
    if (!copy) {
        return httpd_resp_send_500(req);
    }
    size_t len;
    taskENTER_CRITICAL(&s_log_lock);
    if (s_log_wrapped) {
        size_t tail = LOG_BUF_SIZE - s_log_head;
        memcpy(copy, s_log + s_log_head, tail);
        memcpy(copy + tail, s_log, s_log_head);
        len = LOG_BUF_SIZE;
    } else {
        memcpy(copy, s_log, s_log_head);
        len = s_log_head;
    }
    taskEXIT_CRITICAL(&s_log_lock);
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    esp_err_t err = httpd_resp_send(req, copy, len);
    free(copy);
    return err;
}

static void restart_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
}

static esp_err_t ota_post(httpd_req_t *req)
{
    if (!http_authorized(req)) {
        httpd_resp_set_status(req, "401 Unauthorized");
        return httpd_resp_sendstr(req, "bad or missing token\n");
    }
    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    if (!part || req->content_len == 0 || req->content_len > part->size) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing or oversized image");
    }
    ESP_LOGI(TAG, "OTA: receiving %u bytes into %s", req->content_len, part->label);
    esp_ota_handle_t ota = 0;
    esp_err_t err = esp_ota_begin(part, OTA_WITH_SEQUENTIAL_WRITES, &ota);
    if (err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, esp_err_to_name(err));
    }
    char *buf = malloc(4096);
    size_t remaining = req->content_len;
    bool first = true;
    int timeouts = 0;
    while (buf && remaining > 0) {
        int n = httpd_req_recv(req, buf, remaining < 4096 ? remaining : 4096);
        if (n == HTTPD_SOCK_ERR_TIMEOUT && ++timeouts < 3) {
            continue;
        }
        if (n <= 0) {
            err = ESP_FAIL;
            break;
        }
        if (first && (uint8_t)buf[0] != 0xE9) {  // app images start with ESP_IMAGE_HEADER_MAGIC
            err = ESP_ERR_INVALID_ARG;
            break;
        }
        first = false;
        if ((err = esp_ota_write(ota, buf, n)) != ESP_OK) {
            break;
        }
        remaining -= n;
    }
    free(buf);
    if (!buf) {
        err = ESP_ERR_NO_MEM;
    }
    if (err == ESP_OK) {
        err = esp_ota_end(ota);  // also validates the image
    } else {
        esp_ota_abort(ota);
    }
    if (err == ESP_OK) {
        err = esp_ota_set_boot_partition(part);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA failed: %s", esp_err_to_name(err));
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, esp_err_to_name(err));
    }
    ESP_LOGI(TAG, "OTA: done, rebooting into %s", part->label);
    httpd_resp_sendstr(req, "ok, rebooting\n");
    xTaskCreate(restart_task, "restart", 2048, NULL, 5, NULL);
    return ESP_OK;
}

static esp_err_t reboot_post(httpd_req_t *req)
{
    if (!http_authorized(req)) {
        httpd_resp_set_status(req, "401 Unauthorized");
        return httpd_resp_sendstr(req, "bad or missing token\n");
    }
    httpd_resp_sendstr(req, "rebooting\n");
    xTaskCreate(restart_task, "restart", 2048, NULL, 5, NULL);
    return ESP_OK;
}

static void http_start(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.stack_size = 6144;
    cfg.recv_wait_timeout = 10;
    httpd_handle_t server = NULL;
    ESP_ERROR_CHECK(httpd_start(&server, &cfg));
    const httpd_uri_t routes[] = {
        {.uri = "/status", .method = HTTP_GET, .handler = status_get},
        {.uri = "/", .method = HTTP_GET, .handler = status_get},
        {.uri = "/log", .method = HTTP_GET, .handler = log_get},
        {.uri = "/ota", .method = HTTP_POST, .handler = ota_post},
        {.uri = "/reboot", .method = HTTP_POST, .handler = reboot_post},
    };
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        httpd_register_uri_handler(server, &routes[i]);
    }
}

// ---------------------------------------------------------------- main

void app_main(void)
{
    esp_log_set_vprintf(log_vprintf);
    ESP_LOGI(TAG, "wyze-esp-bridge %s (" BOARD_NAME ")", esp_app_get_description()->version);

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
    config_load();
    console_setup();

    s_events = xEventGroupCreate();
    s_hid_driver_q = xQueueCreate(4, sizeof(hid_driver_msg_t));
    s_to_gateway_q = xQueueCreate(16, sizeof(frame_t));
    s_to_dongle_q = xQueueCreate(16, sizeof(frame_t));

    ESP_ERROR_CHECK(esp_event_loop_create_default());
    eth_start();
    http_start();

    xTaskCreatePinnedToCore(usb_lib_task, "usb_lib", 4096, xTaskGetCurrentTaskHandle(), 2, NULL, 0);
    ulTaskNotifyTake(pdFALSE, pdMS_TO_TICKS(1000));
    const hid_host_driver_config_t hid_cfg = {
        .create_background_task = true,
        .task_priority = 5,
        .stack_size = 4096,
        .core_id = 0,
        .callback = hid_driver_cb,
        .callback_arg = NULL,
    };
    ESP_ERROR_CHECK(hid_host_install(&hid_cfg));
    xTaskCreate(to_dongle_task, "to_dongle", 4096, NULL, 5, NULL);
    xTaskCreate(to_gateway_task, "to_gateway", 4096, NULL, 5, NULL);
    ESP_LOGI(TAG, "waiting for Wyze dongle");

    // Open the gateway connection only while the dongle is present and the network is up,
    // so the gateway sees a disconnect when the dongle is unplugged.
    while (true) {
        hid_driver_msg_t msg;
        if (xQueueReceive(s_hid_driver_q, &msg, pdMS_TO_TICKS(500)) == pdTRUE &&
            msg.event == HID_HOST_DRIVER_EVENT_CONNECTED) {
            open_hid_device(msg.handle);
        }
        bool net_up = xEventGroupGetBits(s_events) & NET_UP_BIT;
        if (s_dongle && net_up && !s_ws) {
            ws_start();
        } else if ((s_dongle_gone || !s_dongle) && s_ws) {
            ws_stop();
            s_dongle_gone = false;
        }
    }
}
