// wyze-esp-bridge: relays a Wyze Sense Bridge dongle (USB 1a86:e024) to a
// wyzesense2mqtt-rs gateway over WebSocket, like the gateway's dongle_bridge.
//
// Boards (selected by IDF target):
//   esp32p4: Waveshare ESP32-P4-WIFI6-POE-ETH. Internal EMAC + IP101 PHY; dongle on the USB-A
//            port (USB 2.0 HS controller); console on UART0 via the USB-C port's CH343.
//   esp32s3: Waveshare ESP32-S3-ETH. W5500 SPI Ethernet; dongle on the USB-C port through an OTG
//            adapter; console on UART0 header pins GPIO43 TX / GPIO44 RX. The same image runs
//            Wi-Fi-only on a bare S3 board (e.g. DevKitC-1): a missing W5500 is not fatal.
//
// Network: Ethernet is preferred whenever it has an IP; Wi-Fi (if an SSID is set) covers the
// gaps and is stopped once Ethernet gets an address. Never both in use at once.
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
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "soc/soc_caps.h"
#include "usb/hid_host.h"
#include "usb/usb_host.h"

// Wi-Fi: the S3 has its own radio; the P4 drives the board's ESP32-C6 over SDIO (esp_hosted).
#if (CONFIG_IDF_TARGET_ESP32P4 && CONFIG_ESP_HOSTED) || SOC_WIFI_SUPPORTED
#define HAS_WIFI 1
#include "esp_wifi.h"
#if CONFIG_ESP_HOSTED
#include "esp_wifi_remote.h"
#endif
#else
#define HAS_WIFI 0
#endif

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

#define LOG_BUF_SIZE 49152

static char s_log[LOG_BUF_SIZE];
static uint64_t s_log_written;  // total bytes ever written; s_log[s_log_written % LOG_BUF_SIZE] is next
static portMUX_TYPE s_log_lock = portMUX_INITIALIZER_UNLOCKED;

// Masks the value of every "token=" in a log line. The gateway URI carries the bridge token in
// its query string, and component code (e.g. the WebSocket client's "Error parse uri = %s")
// may log that URI; /log is served without auth.
static bool log_redact_token(char *line)
{
    bool redacted = false;
    for (char *p = strstr(line, "token="); p; p = strstr(p, "token=")) {
        p += 6;
        char *end = p;
        while (*end && *end != '&' && *end != '"' && *end != ' ' && *end != '\r' && *end != '\n') {
            end++;
        }
        if (end - p >= 3) {
            memmove(p + 3, end, strlen(end) + 1);  // shrinks or keeps the length: never overflows
            memcpy(p, "***", 3);
            p += 3;
            redacted = true;
        } else if (end > p) {
            memset(p, '*', end - p);
            p = end;
            redacted = true;
        }
    }
    return redacted;
}

static int log_vprintf(const char *fmt, va_list args)
{
    char line[256];
    va_list copy;
    va_copy(copy, args);
    int n = vsnprintf(line, sizeof(line), fmt, copy);
    va_end(copy);
    if (n <= 0) {
        return vprintf(fmt, args);
    }
    const bool redacted = log_redact_token(line);
    const size_t len = strlen(line);
    taskENTER_CRITICAL(&s_log_lock);
    size_t idx = s_log_written % LOG_BUF_SIZE;
    for (size_t i = 0; i < len; i++) {
        s_log[idx++] = line[i];
        if (idx == LOG_BUF_SIZE) {
            idx = 0;
        }
    }
    s_log_written += len;
    taskEXIT_CRITICAL(&s_log_lock);
    // Print the redacted copy (possibly truncated) rather than leak the token on the UART too.
    return redacted ? printf("%s", line) : vprintf(fmt, args);
}

// ---------------------------------------------------------------- config

typedef struct {
    char uri[128];
    char token[65];
    char wifi_ssid[33];
    char wifi_pass[65];
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
    // Baked defaults first: a fresh flash has no "bridge" namespace yet, and the
    // baked Wi-Fi credentials are what gets such a board on the network.
    strlcpy(s_cfg.uri, CONFIG_WYZE_GATEWAY_URI, sizeof(s_cfg.uri));
    strlcpy(s_cfg.token, CONFIG_WYZE_BRIDGE_TOKEN, sizeof(s_cfg.token));
    strlcpy(s_cfg.wifi_ssid, CONFIG_WYZE_WIFI_SSID, sizeof(s_cfg.wifi_ssid));
    strlcpy(s_cfg.wifi_pass, CONFIG_WYZE_WIFI_PASS, sizeof(s_cfg.wifi_pass));
    nvs_handle_t h;
    if (nvs_open("bridge", NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    nvs_get_str_or(h, "uri", s_cfg.uri, sizeof(s_cfg.uri), CONFIG_WYZE_GATEWAY_URI);
    nvs_get_str_or(h, "token", s_cfg.token, sizeof(s_cfg.token), CONFIG_WYZE_BRIDGE_TOKEN);
    nvs_get_str_or(h, "wifi_ssid", s_cfg.wifi_ssid, sizeof(s_cfg.wifi_ssid), CONFIG_WYZE_WIFI_SSID);
    nvs_get_str_or(h, "wifi_pass", s_cfg.wifi_pass, sizeof(s_cfg.wifi_pass), CONFIG_WYZE_WIFI_PASS);
    nvs_close(h);
}

static esp_err_t config_save(void)
{
    nvs_handle_t h;
    ESP_RETURN_ON_ERROR(nvs_open("bridge", NVS_READWRITE, &h), TAG, "nvs_open");
    esp_err_t err = nvs_set_str(h, "uri", s_cfg.uri);
    if (err == ESP_OK) {
        err = nvs_set_str(h, "token", s_cfg.token);
    }
    if (err == ESP_OK) {
        err = nvs_set_str(h, "wifi_ssid", s_cfg.wifi_ssid);
    }
    if (err == ESP_OK) {
        err = nvs_set_str(h, "wifi_pass", s_cfg.wifi_pass);
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

// Setup-dialog input deadline: a prompt left unanswered this long (e.g. it was entered by line
// noise on a headless board) aborts the dialog and boot continues with the current values.
#define CONSOLE_IDLE_MS 60000

// Reads a line from the console UART. Returns false if no complete line arrives within
// idle_timeout of the start or of the last keystroke. Non-printable bytes (0x00 etc. from
// USB-UART line toggles when a terminal attaches mid-boot) are ignored and do not extend the
// deadline, so line noise can never hang boot.
static bool console_read_line(char *out, size_t size, bool secret, TickType_t idle_timeout)
{
    size_t n = 0;
    TickType_t deadline = xTaskGetTickCount() + idle_timeout;
    while (true) {
        const TickType_t now = xTaskGetTickCount();
        if ((int32_t)(deadline - now) <= 0) {
            out[n] = '\0';
            return false;
        }
        uint8_t c;
        if (uart_read_bytes(CONSOLE_UART, &c, 1, deadline - now) != 1) {
            continue;
        }
        if (c == '\r' || c == '\n') {
            if (n == 0 && c == '\n') {
                continue;  // swallow the LF of a CRLF pair
            }
            break;
        }
        if ((c == 0x08 || c == 0x7f) && n > 0) {
            n--;
            uart_write_bytes(CONSOLE_UART, "\b \b", 3);
        } else if (c >= 0x20 && c < 0x7f && n + 1 < size) {
            out[n++] = (char)c;
            uart_write_bytes(CONSOLE_UART, secret ? "*" : (const char *)&c, 1);
        } else {
            continue;
        }
        deadline = xTaskGetTickCount() + idle_timeout;
    }
    out[n] = '\0';
    uart_write_bytes(CONSOLE_UART, "\r\n", 2);
    return true;
}

// Waits up to timeout for Enter (CR or LF). Every other byte is ignored, so the boot window
// stays exactly timeout long whatever arrives on the line.
static bool console_wait_enter(TickType_t timeout)
{
    const TickType_t deadline = xTaskGetTickCount() + timeout;
    while (true) {
        const TickType_t now = xTaskGetTickCount();
        if ((int32_t)(deadline - now) <= 0) {
            return false;
        }
        uint8_t c;
        if (uart_read_bytes(CONSOLE_UART, &c, 1, deadline - now) == 1 && (c == '\r' || c == '\n')) {
            return true;
        }
    }
}

// Enter keeps the current value; "-" clears it (when can_clear). Returns false on timeout.
static bool prompt(const char *label, char *field, size_t size, bool secret, bool can_clear)
{
    char line[128];
    const char *shown = secret ? (field[0] ? "<unchanged>" : "") : field;
    printf("%s [%s]: ", label, shown);
    fflush(stdout);
    if (!console_read_line(line, sizeof(line), secret, pdMS_TO_TICKS(CONSOLE_IDLE_MS))) {
        printf("\nNo input for %d s, setup cancelled (nothing saved).\n", CONSOLE_IDLE_MS / 1000);
        return false;
    }
    if (can_clear && strcmp(line, "-") == 0) {
        field[0] = '\0';
    } else if (line[0]) {
        if (strlcpy(field, line, size) >= size) {
            printf("Warning: truncated to %u characters.\n", (unsigned)(size - 1));
        }
    }
    return true;
}

// Built-in defaults work as-is, so setup is only offered, never required.
static void console_setup(void)
{
    ESP_ERROR_CHECK(uart_driver_install(CONSOLE_UART, 256, 0, 0, NULL, 0));
    printf("\nGateway %s\nPress Enter within 3 s to change settings...\n", s_cfg.uri);
    if (!console_wait_enter(pdMS_TO_TICKS(3000))) {
        return;
    }
    const bridge_config_t saved = s_cfg;
    printf("\n=== wyze-esp-bridge setup (Enter keeps the value in brackets, \"-\" clears it) ===\n");
    if (!prompt("Gateway URI", s_cfg.uri, sizeof(s_cfg.uri), false, false) ||
        !prompt("Bridge token", s_cfg.token, sizeof(s_cfg.token), true, true) ||
        !prompt("WiFi SSID (used when Ethernet has no IP, \"-\" = off)", s_cfg.wifi_ssid, sizeof(s_cfg.wifi_ssid),
                false, true) ||
        !prompt("WiFi password", s_cfg.wifi_pass, sizeof(s_cfg.wifi_pass), true, true)) {
        s_cfg = saved;
        return;
    }
    if (s_cfg.wifi_ssid[0] == '\0') {
        s_cfg.wifi_pass[0] = '\0';  // no SSID: drop any stale password
    }
    esp_err_t err = config_save();
    if (err != ESP_OK) {
        // Keep running with the values entered; they are lost on the next reboot.
        printf("Saving failed (%s); using the new values until the next reboot.\n", esp_err_to_name(err));
        return;
    }
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

#define ETH_LINK_BIT BIT0
#define ETH_IP_BIT BIT1
#define WIFI_IP_BIT BIT2

typedef enum { NET_NONE, NET_ETH, NET_WIFI } net_kind_t;

static EventGroupHandle_t s_events;
static QueueHandle_t s_hid_driver_q;  // new HID devices (driver callback -> app task)
static QueueHandle_t s_to_gateway_q;  // dongle -> gateway frames
static QueueHandle_t s_to_dongle_q;   // gateway -> dongle frames
// s_dongle_lock guards s_dongle and s_dongle_gone, and is held across every SET_REPORT so the
// HID driver cannot tear the device down underneath an in-flight control transfer.
static SemaphoreHandle_t s_dongle_lock;
static hid_host_device_handle_t s_dongle;
static bool s_dongle_gone;
// s_ws_lock guards s_ws for tasks other than the main loop (the only writer): it is held across
// every use of the client from to_gateway_task and the HTTP server, and taken by ws_stop()
// before the client is destroyed.
static SemaphoreHandle_t s_ws_lock;
static esp_websocket_client_handle_t s_ws;
static esp_netif_t *s_netif;       // Ethernet; NULL when no Ethernet hardware answered
static esp_netif_t *s_wifi_netif;  // NULL unless a Wi-Fi SSID is configured
static bool s_ws_bad_uri;
static volatile bool s_gateway_seen;  // set on the first WebSocket CONNECTED; read by the main loop
static volatile uint32_t s_frames_up, s_frames_down;

// ---------------------------------------------------------------- Ethernet / WiFi

// Wi-Fi is (re)started from a one-shot esp_timer, never from a delay inside an event
// handler: the default event loop is shared with lwIP/websocket/USB events. One-shot (not
// periodic) and restarted on every arm so bursts of DISCONNECTED don't stack up.
#if HAS_WIFI
#define WIFI_RETRY_US (5 * 1000000)
#define WIFI_BOOT_GRACE_US (8 * 1000000)  // let Ethernet link + DHCP first at boot
#define WIFI_LINKDOWN_US (1 * 1000000)    // Ethernet link or address lost -> start Wi-Fi
#define WIFI_STOP_US 1000                 // Ethernet is back -> stop Wi-Fi, outside the event loop
static esp_timer_handle_t s_wifi_retry_timer;
static volatile bool s_wifi_running;  // between STA_START and STA_STOP

static void wifi_retry_timer_cb(void *arg)
{
    // Ethernet-first: once it has an address the WLAN rests (link-down restarts it). The stop
    // happens here rather than in net_event_handler: on the P4 esp_wifi_stop() is a blocking
    // RPC to the C6 and must not stall the shared default event loop.
    if (xEventGroupGetBits(s_events) & ETH_IP_BIT) {
        if (s_wifi_running) {
            ESP_LOGI(TAG, "wifi stopped (ethernet is primary)");
            esp_wifi_stop();
        }
        return;
    }
    if (s_wifi_running) {
        esp_wifi_connect();
    } else {
        ESP_LOGI(TAG, "starting wifi (ssid: %s, ethernet has no IP)", s_cfg.wifi_ssid);
        esp_wifi_start();  // STA_START handler connects
    }
}

static void wifi_arm(uint64_t delay_us)
{
    if (!s_wifi_netif) {
        return;
    }
    esp_timer_stop(s_wifi_retry_timer);
    esp_timer_start_once(s_wifi_retry_timer, delay_us);  // MICROSECONDS, not ticks
}
#endif

static void net_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == ETH_EVENT && id == ETHERNET_EVENT_CONNECTED) {
        // Wi-Fi keeps carrying traffic until Ethernet actually has an IP: a cable into a
        // dead port or a switch without DHCP must not take the board offline.
        ESP_LOGI(TAG, "ethernet link up (uptime_s=%lld)", (long long)(esp_timer_get_time() / 1000000));
        xEventGroupSetBits(s_events, ETH_LINK_BIT);
    } else if (base == ETH_EVENT && id == ETHERNET_EVENT_DISCONNECTED) {
        // Diagnostics for the periodic ~25.5 min session-kill: prove whether
        // the link physically drops (PHY EEE/PoE class) vs the TCP dying
        // inside a healthy link. Uptime+frames pin the cause to this event.
        ESP_LOGW(TAG, "ethernet link down (uptime_s=%lld frames=%lu/%lu)",
                 (long long)(esp_timer_get_time() / 1000000), (unsigned long)s_frames_up,
                 (unsigned long)s_frames_down);
        xEventGroupClearBits(s_events, ETH_LINK_BIT | ETH_IP_BIT);
#if HAS_WIFI
        wifi_arm(WIFI_LINKDOWN_US);
#endif
    } else if (base == IP_EVENT && id == IP_EVENT_ETH_LOST_IP) {
        // DHCP lease gone with the link still up (e.g. the DHCP server is down).
        ESP_LOGW(TAG, "ethernet lost its IP");
        xEventGroupClearBits(s_events, ETH_IP_BIT);
#if HAS_WIFI
        wifi_arm(WIFI_LINKDOWN_US);
#endif
    } else if (base == IP_EVENT && id == IP_EVENT_ETH_GOT_IP) {
        ip_event_got_ip_t *ev = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "network up (ethernet), IP " IPSTR, IP2STR(&ev->ip_info.ip));
        xEventGroupSetBits(s_events, ETH_IP_BIT);
#if HAS_WIFI
        wifi_arm(WIFI_STOP_US);  // replaces any pending start; stops Wi-Fi if it is running
#endif
    }
#if HAS_WIFI
    else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        s_wifi_running = true;
        ESP_LOGI(TAG, "wifi sta started; connecting to %s", s_cfg.wifi_ssid);
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_STOP) {
        s_wifi_running = false;
        xEventGroupClearBits(s_events, WIFI_IP_BIT);
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        // Also fires for our own esp_wifi_stop(); only touch the Wi-Fi bit, and only
        // retry while Ethernet has no address.
        xEventGroupClearBits(s_events, WIFI_IP_BIT);
        if (!(xEventGroupGetBits(s_events) & ETH_IP_BIT)) {
            const wifi_event_sta_disconnected_t *ev = (const wifi_event_sta_disconnected_t *)data;
            // 202 = auth failed (wrong password), 201/211 = AP not found
            ESP_LOGW(TAG, "wifi disconnected (reason %d), retrying in 5 s", ev->reason);
            wifi_arm(WIFI_RETRY_US);
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ev = (ip_event_got_ip_t *)data;
        if (xEventGroupGetBits(s_events) & ETH_IP_BIT) {
            ESP_LOGI(TAG, "wifi got IP but ethernet is up; stopping wifi");
            wifi_arm(WIFI_STOP_US);
        } else {
            ESP_LOGW(TAG, "network up (wifi), IP " IPSTR, IP2STR(&ev->ip_info.ip));
            xEventGroupSetBits(s_events, WIFI_IP_BIT);
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_LOST_IP) {
        xEventGroupClearBits(s_events, WIFI_IP_BIT);
    }
#endif
}

// Returns false (without aborting) when no Ethernet controller answers, e.g. the
// s3-eth image on a bare S3 board with no W5500: the board then runs Wi-Fi-only.
static bool eth_start(void)
{
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
    esp_eth_handle_t eth = NULL;
    esp_err_t err = ESP_ERR_NOT_FOUND;  // MAC or PHY driver could not be created
    if (mac && phy) {
        esp_eth_config_t eth_cfg = ETH_DEFAULT_CONFIG(mac, phy);
        err = esp_eth_driver_install(&eth_cfg, &eth);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "no ethernet (%s), wifi only", esp_err_to_name(err));
        if (mac) {
            mac->del(mac);
        }
        if (phy) {
            phy->del(phy);
        }
        return false;
    }

    // Use the chip's factory Ethernet MAC (the W5500 has none of its own).
    uint8_t addr[6];
    ESP_ERROR_CHECK(esp_read_mac(addr, ESP_MAC_ETH));
    ESP_ERROR_CHECK(esp_eth_ioctl(eth, ETH_CMD_S_MAC_ADDR, addr));
    ESP_LOGI(TAG, "ethernet MAC %02x:%02x:%02x:%02x:%02x:%02x", addr[0], addr[1], addr[2], addr[3], addr[4],
             addr[5]);

    esp_netif_config_t netif_cfg = ESP_NETIF_DEFAULT_ETH();
    s_netif = esp_netif_new(&netif_cfg);
    esp_netif_set_hostname(s_netif, HOSTNAME);
    ESP_ERROR_CHECK(esp_netif_attach(s_netif, esp_eth_new_netif_glue(eth)));
    ESP_ERROR_CHECK(esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID, net_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, net_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_LOST_IP, net_event_handler, NULL));
    ESP_ERROR_CHECK(esp_eth_start(eth));
    return true;
}

// Wi-Fi STA (only when an SSID is configured): native radio on the S3, the board's
// ESP32-C6 co-processor over SDIO on the P4.
static void wifi_init(bool have_eth)
{
#if HAS_WIFI
    if (!s_cfg.wifi_ssid[0]) {
        return;
    }
    // The retry timer exists before s_wifi_netif is published: the Ethernet handlers are
    // already live and only check s_wifi_netif before arming it.
    const esp_timer_create_args_t retry_timer_args = {
        .callback = wifi_retry_timer_cb,
        .name = "wifi-retry",
    };
    ESP_ERROR_CHECK(esp_timer_create(&retry_timer_args, &s_wifi_retry_timer));
    wifi_init_config_t wifi_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wifi_cfg));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, net_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, net_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_LOST_IP, net_event_handler, NULL));
    esp_netif_t *netif = esp_netif_create_default_wifi_sta();
#if CONFIG_ESP_HOSTED
    // The hosted port's create_default skips the io-driver binding that installs the
    // wlanif input path (IDF's native one does it): without this every RX frame is
    // dropped ("eh_rx_guard: netif input not attached yet") and DHCP never completes.
    ESP_ERROR_CHECK(esp_netif_attach_wifi_station(netif));
#endif
    ESP_ERROR_CHECK(esp_netif_set_hostname(netif, HOSTNAME));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    wifi_config_t sta_cfg = {0};
    strlcpy((char *)sta_cfg.sta.ssid, s_cfg.wifi_ssid, sizeof(sta_cfg.sta.ssid));
    strlcpy((char *)sta_cfg.sta.password, s_cfg.wifi_pass, sizeof(sta_cfg.sta.password));
    // Scan threshold: WPA_PSK is the weakest PSK mode, so WPA/WPA2/WPA3-mixed APs still pass
    // (a stricter threshold gives reason 211, NO_AP_FOUND_IN_AUTHMODE_THRESHOLD). It must not
    // be OPEN when a password is set: an open AP spoofing the SSID never asks for the password,
    // so the station would join it.
    sta_cfg.sta.threshold.authmode = s_cfg.wifi_pass[0] ? WIFI_AUTH_WPA_PSK : WIFI_AUTH_OPEN;
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_cfg));
    s_wifi_netif = netif;
    if (have_eth) {
        ESP_LOGI(TAG, "wifi fallback armed (ssid: %s), starts if ethernet has no IP in 8 s", s_cfg.wifi_ssid);
        wifi_arm(WIFI_BOOT_GRACE_US);
    } else {
        ESP_LOGI(TAG, "starting wifi (ssid: %s)", s_cfg.wifi_ssid);
        ESP_ERROR_CHECK(esp_wifi_start());
    }
    // Gateway -> dongle traffic is latency-sensitive and tiny; modem sleep only delays it.
    esp_wifi_set_ps(WIFI_PS_NONE);
#else
    (void)have_eth;
#endif
}

static void net_start(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    bool have_eth = eth_start();
    wifi_init(have_eth);
    if (!have_eth && !s_wifi_netif) {
        ESP_LOGE(TAG, "no ethernet and no wifi SSID configured: set one on the console");
    }
}

// Ethernet wins whenever it has an address.
static net_kind_t net_active(void)
{
    EventBits_t bits = xEventGroupGetBits(s_events);
    return (bits & ETH_IP_BIT) ? NET_ETH : (bits & WIFI_IP_BIT) ? NET_WIFI : NET_NONE;
}

// ---------------------------------------------------------------- USB / HID

// The CH9350 sometimes misses the first post-settle enumeration (the single
// boot bounce isn't always enough); a physical replug always heals it. Emulate
// that with exactly ONE extra power bounce. Hard rules from the 2.2.0 crash
// (assert in hub_root_stop, hub.c:888):
//  - never loop: each bounce can leave the HCD port mid-recovery; a bounce
//    landing in that window escalates to an abort
//  - fire late (+8 s after power-on) so the port is in a settled state,
//    past debounce + reset + recovery, before we touch power again
//  - skip entirely once the dongle is up
#if CONFIG_IDF_TARGET_ESP32P4
static void usb_replug_timer_cb(void *arg)
{
    if (s_dongle) {
        return;
    }
    ESP_LOGI(TAG, "dongle still absent, emulating one replug");
    if (usb_host_lib_set_root_port_power(false) == ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(200));
        esp_err_t err = usb_host_lib_set_root_port_power(true);
        if (err != ESP_OK) {
            // No retry (see the rules above): the port stays unpowered until a reboot.
            ESP_LOGE(TAG, "root port power-on failed (%s); dongle needs a reboot", esp_err_to_name(err));
        } else {
            ESP_LOGI(TAG, "root port power bounced (dongle not up)");
        }
    }
}
#endif

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
        // Waits for an in-flight SET_REPORT in to_dongle_task. The driver frees the device
        // after this callback returns, so the transfer must be finished by then. (Its
        // completion is delivered by this same task, so a transfer to a vanished dongle ends
        // by its 5 s timeout instead; that stalls the HID task once, but safely.)
        xSemaphoreTake(s_dongle_lock, portMAX_DELAY);
        if (handle == s_dongle) {
            s_dongle = NULL;
            s_dongle_gone = true;
        }
        xSemaphoreGive(s_dongle_lock);
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
    if (xQueueSend(s_hid_driver_q, &msg, pdMS_TO_TICKS(100)) != pdTRUE) {
        ESP_LOGE(TAG, "HID driver event %d dropped (app task busy); replug the dongle", event);
    }
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
    const esp_timer_create_args_t replug_timer_args = {
        .callback = usb_replug_timer_cb,
        .name = "usb-replug",
        .dispatch_method = ESP_TIMER_TASK,
    };
    esp_timer_handle_t replug_timer;
    ESP_ERROR_CHECK(esp_timer_create(&replug_timer_args, &replug_timer));
    // +8 s after power-on: past debounce(300ms)+reset+recovery windows; the
    // timer cb re-checks s_dongle before bouncing, so an on-time dongle skips it.
    // esp_timer_start_once takes MICROSECONDS, not ticks (see AGENTS.md gotcha 5).
    ESP_ERROR_CHECK(esp_timer_start_once(replug_timer, 8 * 1000000));
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
    if (s_dongle) {
        ESP_LOGW(TAG, "a Wyze dongle is already in use, ignoring the second one");
        return;
    }
    const hid_host_device_config_t dev_config = {.callback = hid_interface_cb, .callback_arg = NULL};
    if (hid_host_device_open(handle, &dev_config) != ESP_OK) {
        ESP_LOGE(TAG, "failed to open Wyze dongle");
        return;
    }
    if (hid_host_device_start(handle) != ESP_OK) {
        ESP_LOGE(TAG, "failed to start Wyze dongle");
        hid_host_device_close(handle);
        return;
    }
    xSemaphoreTake(s_dongle_lock, portMAX_DELAY);
    s_dongle = handle;  // s_dongle_gone stays set until the main loop recycles the WebSocket
    xSemaphoreGive(s_dongle_lock);
    ESP_LOGI(TAG, "Wyze dongle up, connecting to gateway");
}

static void to_dongle_task(void *arg)
{
    frame_t f;
    while (true) {
        if (xQueueReceive(s_to_dongle_q, &f, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        xSemaphoreTake(s_dongle_lock, portMAX_DELAY);
        if (!s_dongle) {
            xSemaphoreGive(s_dongle_lock);
            continue;
        }
        // Mirrors a Linux hidraw write on a device without an OUT endpoint.
        esp_err_t err = hid_class_request_set_report(s_dongle, HID_REPORT_TYPE_OUTPUT, 0, f.data, f.len);
        xSemaphoreGive(s_dongle_lock);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "SET_REPORT (%u bytes) failed: %s", f.len, esp_err_to_name(err));
        } else {
            s_frames_down++;
        }
    }
}

// ---------------------------------------------------------------- WebSocket

static frame_t s_rx;     // reassembly buffer for fragmented messages (WebSocket task only)
static bool s_rx_active;  // inside a binary message: continuation frames (opcode 0) belong to it

static void ws_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    esp_websocket_event_data_t *ev = (esp_websocket_event_data_t *)data;
    switch (id) {
    case WEBSOCKET_EVENT_CONNECTED:
        ESP_LOGI(TAG, "gateway connected");
        s_gateway_seen = true;  // the main loop confirms a new OTA image (ota_confirm_poll)
        xQueueReset(s_to_dongle_q);
        s_rx_active = false;
        break;
    case WEBSOCKET_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "gateway disconnected");
        break;
    case WEBSOCKET_EVENT_DATA:
        // Each WebSocket frame arrives as one or more chunks (payload_offset/data_len within
        // payload_len); a message is one binary frame plus any continuation frames up to FIN.
        // Control frames (ping/pong/close) may be interleaved and are skipped here.
        if (ev->op_code == 0x02 && ev->payload_offset == 0) {
            s_rx.len = 0;
            s_rx_active = true;
        } else if (!s_rx_active || (ev->op_code != 0x02 && ev->op_code != 0x00)) {
            if (ev->op_code == 0x01) {
                s_rx_active = false;  // a text message: only binary frames carry protocol data
            }
            return;
        }
        if (s_rx.len + ev->data_len > MAX_FRAME) {
            ESP_LOGW(TAG, "frame too large (> %d bytes), dropped", MAX_FRAME);
            s_rx_active = false;
            return;
        }
        memcpy(s_rx.data + s_rx.len, ev->data_ptr, ev->data_len);
        s_rx.len += ev->data_len;
        if (ev->fin && ev->payload_offset + ev->data_len >= ev->payload_len) {
            s_rx_active = false;
            if (s_rx.len == 0) {
                return;  // an empty message is not a protocol packet
            }
            if (xQueueSend(s_to_dongle_q, &s_rx, pdMS_TO_TICKS(100)) != pdTRUE) {
                ESP_LOGW(TAG, "dongle queue full, dropped frame");
            }
        }
        break;
    case WEBSOCKET_EVENT_ERROR:
        ESP_LOGW(TAG, "gateway connection error");
        break;
    default:
        break;
    }
}

// Percent-encodes src for a URI query value (RFC 3986 unreserved characters pass through, so
// typical alphanumeric tokens are sent unchanged). Returns false if out is too small.
static bool url_encode(char *out, size_t size, const char *src)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t n = 0;
    for (; *src; src++) {
        const unsigned char c = (unsigned char)*src;
        const bool plain = (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                           c == '-' || c == '.' || c == '_' || c == '~';
        if (n + (plain ? 1 : 3) >= size) {
            return false;
        }
        if (plain) {
            out[n++] = c;
        } else {
            out[n++] = '%';
            out[n++] = hex[c >> 4];
            out[n++] = hex[c & 0xF];
        }
    }
    out[n] = '\0';
    return true;
}

static void ws_start(void)
{
    static char uri[sizeof(s_cfg.uri) + 3 * (sizeof(s_cfg.token) - 1) + 48];
    const char *sep = strchr(s_cfg.uri, '?') ? "&" : "?";
    if (s_cfg.token[0]) {
        char token[3 * (sizeof(s_cfg.token) - 1) + 1];
        url_encode(token, sizeof(token), s_cfg.token);  // sized for the worst case: cannot fail
        snprintf(uri, sizeof(uri), "%s%sdevice=" BOARD_NAME "&token=%s", s_cfg.uri, sep, token);
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
    s_rx_active = false;
    esp_websocket_client_handle_t ws = esp_websocket_client_init(&cfg);
    if (!ws) {
        // Only the console (which reboots) changes the URI: don't retry every 500 ms.
        ESP_LOGE(TAG, "invalid gateway URI \"%s\"; fix it on the console", s_cfg.uri);
        s_ws_bad_uri = true;
        return;
    }
    esp_websocket_register_events(ws, WEBSOCKET_EVENT_ANY, ws_event_handler, NULL);
    esp_websocket_client_start(ws);
    xSemaphoreTake(s_ws_lock, portMAX_DELAY);
    s_ws = ws;
    xSemaphoreGive(s_ws_lock);
    ESP_LOGI(TAG, "connecting to %s", s_cfg.uri);
}

static void ws_stop(void)
{
    if (!s_ws) {
        return;
    }
    // Unpublish under the lock: once it is held no other task is using the client, and
    // afterwards none can pick it up, so destroying it outside the lock is safe.
    xSemaphoreTake(s_ws_lock, portMAX_DELAY);
    esp_websocket_client_handle_t ws = s_ws;
    s_ws = NULL;
    xSemaphoreGive(s_ws_lock);
    esp_websocket_client_stop(ws);
    esp_websocket_client_destroy(ws);
    ESP_LOGI(TAG, "gateway connection closed");
}

static void to_gateway_task(void *arg)
{
    frame_t f;
    while (true) {
        if (xQueueReceive(s_to_gateway_q, &f, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        xSemaphoreTake(s_ws_lock, portMAX_DELAY);
        if (s_ws && esp_websocket_client_is_connected(s_ws)) {
            if (esp_websocket_client_send_bin(s_ws, (const char *)f.data, f.len, pdMS_TO_TICKS(1000)) < 0) {
                ESP_LOGW(TAG, "send to gateway failed");
            } else {
                s_frames_up++;
            }
        }
        xSemaphoreGive(s_ws_lock);
    }
}

// ---------------------------------------------------------------- OTA image confirmation

// With CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE a freshly OTA'd image boots in PENDING_VERIFY, and
// any reset before it is marked valid rolls back to the previous slot. It is marked valid once
// the WebSocket to the gateway connects (dongle, network and gateway all work), not merely on an
// IP. Bound: if the gateway has not connected OTA_CONFIRM_FALLBACK_US after the first IP of this
// boot (gateway down or misconfigured, dongle unplugged), the image is kept anyway, so a gateway
// problem never turns into a rollback. An image that never gets an IP is never confirmed.
// The confirmation runs in the main loop, never in the WebSocket or event-loop tasks.
#define OTA_CONFIRM_FALLBACK_US (5 * 60 * 1000000LL)

static bool ota_pending_verify(void)
{
    esp_ota_img_states_t state;
    return esp_ota_get_state_partition(esp_ota_get_running_partition(), &state) == ESP_OK &&
           state == ESP_OTA_IMG_PENDING_VERIFY;
}

// Called from the main loop while the image is pending. Returns true when done (confirmed, or
// confirmation failed and was logged), false to be polled again.
static bool ota_confirm_poll(net_kind_t net, int64_t *net_since_us)
{
    const char *why = NULL;
    if (s_gateway_seen) {
        why = "gateway connected";
    } else if (net != NET_NONE) {
        const int64_t now = esp_timer_get_time();
        if (*net_since_us < 0) {
            *net_since_us = now;
        } else if (now - *net_since_us >= OTA_CONFIRM_FALLBACK_US) {
            why = "no gateway connection 5 min after the network came up; keeping it anyway";
        }
    }
    if (!why) {
        return false;
    }
    esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "could not mark this firmware valid (%s); the next reset rolls back", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "new firmware marked valid (%s)", why);
    }
    return true;
}

// ---------------------------------------------------------------- HTTP: status, log, OTA

// Compares without an early exit, so the time taken does not reveal how much of a guess matched.
static bool token_equal(const char *given, const char *token)
{
    const size_t n = strlen(token);
    const size_t m = strlen(given);
    unsigned char diff = m != n;
    for (size_t i = 0; i < n; i++) {
        diff |= (unsigned char)((i < m ? given[i] : 0) ^ token[i]);
    }
    return diff == 0;
}

// Decodes %XX and '+' in a query value in place.
static void url_decode(char *s)
{
    char *out = s;
    for (; *s; s++) {
        int hi, lo;
        if (*s == '%' && sscanf(s + 1, "%1x%1x", &hi, &lo) == 2) {
            *out++ = (char)(hi << 4 | lo);
            s += 2;
        } else {
            *out++ = *s == '+' ? ' ' : *s;
        }
    }
    *out = '\0';
}

// Accepts the bridge token as "Authorization: Bearer <token>" or ?token=<token>.
static bool http_authorized(httpd_req_t *req)
{
    if (!s_cfg.token[0]) {
        // Open by default so a fresh flash can be updated before a token is set; builds with
        // CONFIG_WYZE_REQUIRE_TOKEN refuse instead.
#if CONFIG_WYZE_REQUIRE_TOKEN
        return false;
#else
        return true;
#endif
    }
    char buf[96];
    if (httpd_req_get_hdr_value_str(req, "Authorization", buf, sizeof(buf)) == ESP_OK &&
        strncmp(buf, "Bearer ", 7) == 0 && token_equal(buf + 7, s_cfg.token)) {
        return true;
    }
    char query[256];
    char token[200];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
        httpd_query_key_value(query, "token", token, sizeof(token)) != ESP_OK) {
        return false;
    }
    url_decode(token);
    return token_equal(token, s_cfg.token);
}

// Writes src as the contents of a JSON string (without quotes), truncating to fit.
static void json_escape(char *out, size_t size, const char *src)
{
    size_t n = 0;
    for (; *src && n + 7 < size; src++) {
        const unsigned char c = (unsigned char)*src;
        if (c == '"' || c == '\\') {
            out[n++] = '\\';
            out[n++] = c;
        } else if (c < 0x20) {
            n += snprintf(out + n, size - n, "\\u%04x", c);
        } else {
            out[n++] = c;
        }
    }
    out[n] = '\0';
}

static esp_err_t status_get(httpd_req_t *req)
{
    const esp_app_desc_t *app = esp_app_get_description();
    const net_kind_t net = net_active();
    esp_netif_ip_info_t ip = {0};
    if (net != NET_NONE) {
        esp_netif_get_ip_info(net == NET_ETH ? s_netif : s_wifi_netif, &ip);
    }
    xSemaphoreTake(s_ws_lock, portMAX_DELAY);
    const bool ws_connected = s_ws && esp_websocket_client_is_connected(s_ws);
    xSemaphoreGive(s_ws_lock);
    const bool eth_link = xEventGroupGetBits(s_events) & ETH_LINK_BIT;
    char uri[2 * sizeof(s_cfg.uri)];
    json_escape(uri, sizeof(uri), s_cfg.uri);
    char body[1024];
    snprintf(body, sizeof(body),
             "{\"board\":\"" BOARD_NAME "\",\"version\":\"%s\",\"built\":\"%s %s\",\"uptime_s\":%lld,\"reset_reason\":%d,"
             "\"ip\":\"" IPSTR "\",\"gateway\":\"%s\",\"dongle\":%s,\"gateway_connected\":%s,"
             "\"network\":\"%s\",\"ethernet_link\":%s,\"wifi_fallback\":%s,"
             "\"frames_to_gateway\":%lu,\"frames_to_dongle\":%lu,\"free_heap\":%lu,\"partition\":\"%s\"}\n",
             app->version, app->date, app->time, esp_timer_get_time() / 1000000, esp_reset_reason(),
             IP2STR(&ip.ip), uri, s_dongle ? "true" : "false", ws_connected ? "true" : "false",
             net == NET_ETH ? "ethernet" : net == NET_WIFI ? "wifi" : "none", eth_link ? "true" : "false",
             s_wifi_netif ? "true" : "false",
             (unsigned long)s_frames_up,
             (unsigned long)s_frames_down, (unsigned long)esp_get_free_heap_size(),
             esp_ota_get_running_partition()->label);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, body);
}

static esp_err_t log_get(httpd_req_t *req)
{
    // Streamed in small chunks, each copied under the (interrupt-masking) spinlock and sent
    // without it: no 48 KB allocation and no long critical section. Text that is overwritten
    // while the response is being sent is skipped, never sent garbled.
    char chunk[512];
    taskENTER_CRITICAL(&s_log_lock);
    const uint64_t end = s_log_written;
    taskEXIT_CRITICAL(&s_log_lock);
    uint64_t pos = end > LOG_BUF_SIZE ? end - LOG_BUF_SIZE : 0;
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    while (pos < end) {
        size_t n = 0;
        taskENTER_CRITICAL(&s_log_lock);
        if (s_log_written - pos > LOG_BUF_SIZE) {
            pos = s_log_written - LOG_BUF_SIZE;  // the writer lapped us
        }
        while (pos < end && n < sizeof(chunk)) {
            chunk[n++] = s_log[pos++ % LOG_BUF_SIZE];
        }
        taskEXIT_CRITICAL(&s_log_lock);
        if (n > 0 && httpd_resp_send_chunk(req, chunk, n) != ESP_OK) {
            return ESP_FAIL;
        }
    }
    return httpd_resp_send_chunk(req, NULL, 0);
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
    if (ota_pending_verify()) {
        // esp_ota_begin() refuses this state (ESP_ERR_OTA_ROLLBACK_INVALID_STATE); say why.
        httpd_resp_set_status(req, "503 Service Unavailable");
        return httpd_resp_sendstr(req, "running firmware is still on trial (confirmed once the gateway connects, "
                                       "at most 5 min after the network is up); retry later\n");
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
            continue;  // three timeouts in a row abort the upload
        }
        if (n <= 0) {
            err = ESP_FAIL;
            break;
        }
        timeouts = 0;
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
    // Idle or slow clients cannot hold every socket: the least recently used one is closed.
    cfg.lru_purge_enable = true;
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
    s_hid_driver_q = xQueueCreate(8, sizeof(hid_driver_msg_t));
    s_dongle_lock = xSemaphoreCreateMutex();
    s_ws_lock = xSemaphoreCreateMutex();
    s_to_gateway_q = xQueueCreate(16, sizeof(frame_t));
    s_to_dongle_q = xQueueCreate(16, sizeof(frame_t));

    ESP_ERROR_CHECK(esp_event_loop_create_default());
    net_start();
    http_start();
    if (!s_cfg.token[0]) {
#if CONFIG_WYZE_REQUIRE_TOKEN
        ESP_LOGW(TAG, "no bridge token set: /ota and /reboot are disabled until one is set on the console");
#else
        ESP_LOGW(TAG, "no bridge token set: /ota and /reboot are open to the whole LAN");
#endif
    }

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
    // so the gateway sees a disconnect when the dongle is unplugged. The WebSocket
    // client reconnects on its own; a wedged client (task exited, stale handle)
    // is detected via the not-connecting state and torn down so the loop remakes it.
    uint32_t last_frames = UINT32_MAX;
    int wedged_polls = 0;
    net_kind_t ws_net = NET_NONE;
    bool ota_pending = ota_pending_verify();
    int64_t ota_net_since_us = -1;
    if (ota_pending) {
        ESP_LOGW(TAG, "new firmware on trial: kept once the gateway connects (at most 5 min after the "
                      "network is up); a reset before that rolls back");
    }
    while (true) {
        hid_driver_msg_t msg;
        if (xQueueReceive(s_hid_driver_q, &msg, pdMS_TO_TICKS(500)) == pdTRUE &&
            msg.event == HID_HOST_DRIVER_EVENT_CONNECTED) {
            open_hid_device(msg.handle);
        }
        const net_kind_t net = net_active();
        if (ota_pending && ota_confirm_poll(net, &ota_net_since_us)) {
            ota_pending = false;
        }
        // A dongle that went away (even if already replugged) or a switch between
        // Ethernet and Wi-Fi ends the session: the gateway must redo its handshake,
        // and a socket bound to the old interface is dead anyway.
        xSemaphoreTake(s_dongle_lock, portMAX_DELAY);
        const bool restart = s_dongle_gone;
        s_dongle_gone = false;
        xSemaphoreGive(s_dongle_lock);
        if (s_ws && (restart || !s_dongle || net != ws_net)) {
            ws_stop();
        }
        if (!s_ws && s_dongle && net != NET_NONE && !s_ws_bad_uri) {
            ws_start();
            ws_net = net;
            last_frames = UINT32_MAX;
            wedged_polls = 0;
        } else if (s_ws) {
            // Client should be either connected or retrying; both make frame
            // progress or keep the handle busy. If several minutes pass with
            // no frames and no connection, rebuild it from scratch.
            bool connected = esp_websocket_client_is_connected(s_ws);
            if (!connected && s_frames_up + s_frames_down == last_frames) {
                if (++wedged_polls > 240) {  // 240 x 500 ms = ~2 min
                    ESP_LOGW(TAG, "gateway client wedged, rebuilding");
                    wedged_polls = 0;
                    ws_stop();
                }
            } else {
                wedged_polls = 0;
            }
            last_frames = s_frames_up + s_frames_down;
        }
    }
}
