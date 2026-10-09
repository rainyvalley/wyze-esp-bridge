# AGENTS.md

ESP-IDF firmware for a USB-host relay: Wyze Sense Bridge dongle (USB HID `1a86:e024`, CH9350 front-end) plugged into an ESP32's USB host port → WebSocket to a `wyzesense2mqtt-rs` gateway at `/ws/bridge`. It emulates the gateway's own `dongle_bridge` wire protocol, but with network instead of a USB cable run.

## Layout

- `main/main.c` — everything (single file, ~1300 lines), sectioned with banner comments: log ring buffer, NVS config + UART setup dialog, Ethernet/Wi-Fi (per-target), USB host + HID (incl. the one-shot root-port power bounce), WebSocket relay, HTTP server, `app_main`.
- `main/Kconfig.projbuild` — default gateway URI + bridge token + Wi-Fi credentials (baked into the image, overridable in NVS via the console prompt), and `WYZE_REQUIRE_TOKEN` (opt-in: refuse `/ota` and `/reboot` while no token is set).
- `sdkconfig.defaults` + per-board `sdkconfig.defaults.esp32p4` / `.p4-rev1` / `.esp32s3`.
- `partitions.csv` — 16 MB flash, two 3 MB OTA slots with rollback.
- `build.sh` — builds all three variants into `out/` and merges flash images.
- `case/*.scad` — OpenSCAD enclosures (P4 case is current, S3 one is obsolete). Hardware/flash instructions and board pinouts live in `README.md`.

Board variants:

| name (build dir) | target | notes |
|---|---|---|
| `p4` (`build-p4`) | `esp32p4` | chip rev v3.x, internal EMAC + IP101 |
| `p4-rev1` (`build-p4-rev1`) | `esp32p4` | chip rev v0.x/v1.x; adds `sdkconfig.defaults.p4-rev1` |
| `s3-eth` (`build-s3-eth`) | `esp32s3` | W5500 SPI Ethernet + native Wi-Fi fallback; same image runs Wi-Fi-only on a bare DevKitC-1 (missing W5500 is non-fatal) |

The P4 revision split is a hard fork (IDF treats v3.x and v0/v1.x as different hardware): `CONFIG_ESP32P4_SELECTS_REV_LESS_V3=y` + `CONFIG_ESP32P4_REV_MIN_0=y` for rev1. Check `chip revision:` in the boot log to pick the image.

## Build

All variants at once (requires Docker; no IDF needed on the host):

```bash
docker run --rm -v "$PWD":/project -w /project espressif/idf:release-v5.5 ./build.sh
```

Single variant:

```bash
docker run --rm -v /path/to/repo:/project -w /project espressif/idf:release-v5.5 \
  idf.py -B build-p4-rev1 -D SDKCONFIG=build-p4-rev1/sdkconfig \
  -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.p4-rev1" \
  set-target esp32p4 reconfigure build
```

Notes:
- `SDKCONFIG` lives *inside* the build dir, not the project root; each variant has its own.
- `merge_bin` (produces `out/*-merged.bin` for flashing at `0x0`) must run inside the container — `esptool.py` is not installed on this host:
  `python /opt/esp/idf/components/esptool_py/esptool/esptool.py --chip esp32p4 merge_bin -o /project/out/<name>-merged.bin @flash_args` from the build dir.
- `out/` gets both `<name>-merged.bin` (flash at 0x0) and `<name>-ota.bin` (POST to `/ota`).
- This is a git repository (public: github.com/rainyvalley/wyze-esp-bridge). `build*/`, `out/`, `managed_components/`, `sdkconfig`, `sdkconfig.local.defaults` are gitignored (and, except the last, regenerable).
- Managed component versions are pinned **per target**: `dependencies.lock.esp32p4` and `dependencies.lock.esp32s3` (selected in the top-level `CMakeLists.txt` via `DEPENDENCIES_LOCK`). A single shared lock would be re-solved, and rewritten, by every build of the other target.
- **Secrets:** site values (gateway URI, token, Wi-Fi SSID/password) go in the git-ignored `sdkconfig.local.defaults` (appended by `build.sh` when present) or into NVS from the console — never in `sdkconfig.defaults`/`Kconfig.projbuild`, which are public. Images built with `sdkconfig.local.defaults` (`out/`, `build-*/sdkconfig`) contain those secrets in plain text; never attach them to a release or share them.

## Deploy / verify

- OTA: `POST` the `-ota.bin` to `http://<board-ip>/ota` with `Authorization: Bearer <token>`. The token is whatever is in the board's NVS (console prompt) or was baked in via `CONFIG_WYZE_BRIDGE_TOKEN` in the git-ignored `sdkconfig.local.defaults`; it must match the gateway's auth. With no token set, `/ota` and `/reboot` are open to the LAN unless built with `CONFIG_WYZE_REQUIRE_TOKEN=y`. There is no `curl` on this host — use `python3 urllib` (see example in git history of this doc's era: build request with `data=` and Bearer header).
- `GET /status` and `GET /log` are unauthenticated; `/log` is a 48 KB ring buffer of boot-up-to-now for **this boot only** (streamed in chunks; `token=` values are masked before they reach the ring or the UART).
- Console: P4 board exposes UART0 on the USB-C CH343 (`/dev/ttyACM0`, 115200). S3 needs a header adapter (GPIO43/44) because its USB-C port is the USB *host*.
- Flash over USB: `pipx run esptool --chip esp32p4 -p /dev/ttyACM0 write_flash 0x0 out/wyze-esp-bridge-p4-rev1-merged.bin` (works with the dongle plugged in; esptool auto-resets into download mode). esptool v5 deprecation warning: the command is now `write-flash`.

## Data flow

- Dongle → host: 64-byte HID input reports. `raw[0]` is a length prefix (`0x3F` max) with the protocol payload following; first bytes `0x55`/`0xAA` mean "whole report is the payload". Each report becomes one binary WebSocket frame (only the protocol bytes).
- Gateway → dongle: one binary WS frame = one protocol packet, written via `hid_class_request_set_report` (Output, report ID 0) — the dongle has **no interrupt OUT endpoint**. `open_hid_device` reads the report descriptor once (under `s_dongle_lock`) and `hid_output_report_len` derives the Output report size (`s_dongle_out_len`; falls back to `HID_REPORT_LEN` = 64 on report IDs, Push/Pop, or a failed read). `to_dongle_task` drops larger packets with a warning instead of sending them.
- Main loop only opens the WebSocket while `s_dongle` is set **and** the network is up, so the gateway sees a disconnect when the dongle is unplugged. Frames flow through `s_to_gateway_q`/`s_to_dongle_q` queues (16 × `frame_t`).
- The gateway (wyzesense2mqtt-rs, on port 8080) authenticates the bridge via `?token=` and labels it with `device=<BOARD_NAME>`; its `/api/dongles` shows bridge sessions and their sensors. `sensor_count: 0` just means nothing paired yet.
- **`/status` `dongle`/`gateway_connected` flags can be stale/wrong while the relay actually works** — the gateway's `/api/dongle` (or `/api/dongles`) is the source of truth for whether the bridge session is live (its presence + completed handshake = working pipeline). Trust `/status` for IP/heap/uptime only. This repo is public now (github.com/rainyvalley/wyze-esp-bridge); the gateway URI/token in `sdkconfig.defaults`/`Kconfig.projbuild` are placeholders — real values live in the board's NVS, set at the console prompt.

## Gotchas

1. **`espressif/usb: "~1.4.1"` pin in `main/idf_component.yml` is load-bearing.** On IDF 5.x the built-in `components/usb` has a root-port disconnect race (`hub.c` `dev_tree_node_dev_gone` → `ESP_ERR_NOT_FOUND` → unguarded `ESP_ERROR_CHECK` → abort at `hub.c:435` and reboot storm), upstream bug [esp-idf#18366](https://github.com/espressif/esp-idf/issues/18366), fixed in esp-usb [PR #485](https://github.com/espressif/esp-usb/pull/485) and first released in esp-usb 1.4.1 (1.5.0 needs IDF ≥ 5.5.3). On IDF ≥6 the `usb_host_hid` component pulls `espressif/usb` itself and this pin is redundant but harmless. If `Hub: Device tree node (parent_port=0): not found` + abort appears, this pin is missing/regressed.
2. **USB timing overrides in `sdkconfig.defaults`** (`USB_HOST_DEBOUNCE_DELAY_MS=300`, `USB_HOST_RESET_HOLD_MS=50`, `USB_HOST_RESET_RECOVERY_MS=250` vs IDF defaults 250/30/30): the CH9350 front-end needs longer-than-spec post-reset settle without generating a boot-time reset-fail loop. Don't remove them when linting `sdkconfig.defaults`.
3. **P4 boot-time USB power sequencing in `usb_lib_task`**: `usb_host_install` runs with `.root_port_unpowered = true`, the port stays off for a 1 s settle, then is powered on; a one-shot esp_timer (`usb_replug_timer_cb`, +8 s) emulates one replug if the dongle is still absent. (There is no PHY/pulldown override and no `skip_phy_setup` in this code.)
4. **"Root port reset failed" ~1 Hz spam with no other USB events** means the CH9350 never pulls the bus (not driven/enumerated at all). Check the dongle is physically in the P4's USB-A port and getting 5 V; also note the P4 board mis-enumerates a device that was already powered during host boot activity — main.c bounces root-port power at boot and once more from the one-shot replug timer (item 3).
5. **USB root-port power bouncing has an abort window.** A power-off/on that lands while the HCD port is mid-recovery makes `POWER_ON` fail; esp-usb's `hub_root_start` error-rollback calls `hub_root_stop()`, whose `assert(ret == ESP_OK)` (hub.c:888) aborts (seen as `assert failed: hub_root_stop` in 2.2.0). Rules: never loop bounces; the replug-emulation timer must fire well clear of debounce/reset/recovery windows (8 s); exactly one retry. Also: `esp_timer_start_once` takes **microseconds** — passing `pdMS_TO_TICKS(x)` schedules a bounce 100× too early (the 2.2.3 bug; fired at 0.8 s instead of 8 s).
6. Firmware **version is `PROJECT_VER` in `CMakeLists.txt`**, not derived from git tags (tags `v*` exist and make CI attach the binaries to a GitHub release, so keep them in step). OTA-rollback is armed by default: a new image is marked valid from the main loop (`ota_confirm_poll`) once the gateway WebSocket connects, or 5 min (`OTA_CONFIRM_FALLBACK_US`) after the first IP if the gateway never does (so a bad gateway config cannot cause a rollback). Any reset before that rolls back; an image that never gets an IP is never confirmed. `/ota` returns 503 while the running image is still pending (`esp_ota_begin` would refuse anyway). No anti-rollback/downgrade check, by decision.
7. The 3-second "Press Enter within 3 s" console prompt at boot is the setup dialog (gateway URI, token, Wi-Fi SSID/password, stored in NVS `bridge` namespace). Only CR/LF opens it; the window is always exactly 3 s. In the dialog Enter keeps a value and `-` clears it. Line noise can still open the dialog, so every prompt has a 60 s inactivity deadline: on timeout the dialog is cancelled without saving and boot continues (it used to wait forever, hanging a headless board).
8. There is **no test suite**; verification is live-device probing: `/status`, `/log`, `pipx run esptool` for raw flashing, and the gateway HTTP API. Log with `TAG "wyze-bridge"`; other tasks log with their own tags (e.g. `HUB` is the usb driver's, `eh_*` are esp-hosted's).
9. Component version updates go through the ESP component registry during `idf.py reconfigure`; the container has network access. If `managed_components/` was deleted, reconfigure re-fetches.
10. **Wi-Fi fallback specifics (P4 `CONFIG_ESP_HOSTED` builds).** The Kconfig symbol is `CONFIG_ESP_HOSTED` — `CONFIG_ESP_HOSTED_ENABLED` does not exist and silently breaks the build. Guard macro in main.c: `HAS_WIFI` (= P4+`CONFIG_ESP_HOSTED`, or `SOC_WIFI_SUPPORTED` for the S3's native radio); hosted-only bits use `#if CONFIG_ESP_HOSTED`. The managed-component manifest can't use a top-level `rules:` key (only per-dependency version+rules). SDIO wiring needs zero customization on this Waveshare board (slot 1 defaults, pins 18/19/14-17, reset 54 = ESP32-P4-Function-EV-Board-compatible).
11. **Wi-Fi RX needs an explicit io-driver attach.** On P4/hosted only (native S3 must NOT double-attach): the hosted port's `esp_netif_create_default_wifi_sta()` does NOT call `esp_netif_attach_wifi_station()` (IDF's local version does) — without the extra `esp_netif_attach_wifi_station(s_wifi_netif)` call, every incoming frame is dropped by the ESP-Hosted glue with `eh_rx_guard: sta: netif input not attached yet` forever (no DHCP, no IP; association itself looks fine). This cost a whole debugging session in 2.1.2 → 2.1.3.
12. **Wi-Fi disconnect reason 211** (`NO_AP_FOUND_IN_AUTHMODE_THRESHOLD`) means the scan *threshold* filtered the AP out (authmode stricter than AP's advertised suite), NOT a wrong password (that's 202). The threshold is `WIFI_AUTH_WPA_PSK` (the weakest PSK mode, so WPA/WPA2/WPA3-mixed APs pass) when a password is set, and `WIFI_AUTH_OPEN` only when it is empty. Do not set it to OPEN with a password: the password is **not** enforced against an open AP that broadcasts the same SSID (it never asks for one), so an evil twin would capture the board.
13. The default event loop is shared; event handlers must never `vTaskDelay` or make blocking calls (Wi-Fi start/retry/stop all run from the `wifi-retry` esp_timer one-shot instead; on the P4 `esp_wifi_*` calls are RPCs to the C6).
14. **Locking:** `s_ws` is only written by the main loop; other tasks (`to_gateway_task`, `/status`) use it only while holding `s_ws_lock`, and `ws_stop()` unpublishes it under that lock before destroying the client. `s_dongle` is guarded by `s_dongle_lock`, held across every SET_REPORT; the HID disconnect callback takes it before the driver frees the device. OTA over the Wi-Fi fallback works (POST /ota on the WiFi IP); note the board's WiFi IP from the gateway session's `remote_addr` if DNS/hostname lookup fails.