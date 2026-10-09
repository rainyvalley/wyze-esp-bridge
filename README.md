# wyze-esp-bridge

ESP32 USB host relay for [wyzesense2mqtt-rs](https://github.com/HclX/wyzesense2mqtt-rs): plug the
Wyze Sense Bridge dongle (USB `1a86:e024`) into the ESP32's USB host port, and the sensors end up on
your MQTT broker via the gateway — with the ESP32 on Ethernet *or Wi-Fi*, anywhere in the house, not
tethered to a USB port on the gateway machine. This project is a hardware counterpart to the
gateway's desktop `dongle_bridge` binary: it implements the same `/ws/bridge` wire protocol, but
the "cable" is a WebSocket.

A [Waveshare ESP32-P4-WIFI6-POE-ETH](https://www.waveshare.com/esp32-p4-wifi6-poe-eth.htm) board is the
recommended host: its USB-A port speaks the USB 2.0 host role the dongle needs, its internal
Ethernet keeps the link wired, and its ESP32-C6 co-processor provides Wi-Fi as an automatic
fallback when the cable is out. Waveshare also makes the S3-ETH PoE board
([wiki](https://www.waveshare.com/esp32-s3-eth.htm)); a plain ESP32-S3-DevKitC-1 is the cheapest
option and runs the same code Wi-Fi-only. See the matrix below.

All three boards run the **same firmware** — pick by connectivity:

| Build | Board | Ethernet | Wi-Fi | Dongle port | Console | Rough price |
|---|---|---|---|---|---|---|
| `p4` / `p4-rev1` | Waveshare **ESP32-P4-WIFI6-POE-ETH** — [Amazon](https://www.amazon.com/dp/B0GFJQSN9B) | yes (internal EMAC + IP101) | Wi-Fi 6 fallback (ESP32-C6 over SDIO) | **USB-A** | USB-C (CH343) | ~$25 |
| `s3-eth` | Waveshare ESP32-S3-ETH PoE — [Amazon](https://a.co/d/0dSdbcFS) | yes (W5500 over SPI) | fallback (S3's own radio) | USB-C + OTG adapter | Header GPIO43/44 | ~$30 |
| `s3-eth` (bare) | ESP32-S3-DevKitC-1 N16R8 — [Amazon](https://a.co/d/04rXdiuX) | — | **Wi-Fi only** | "USB" port + OTG adapter | "UART" USB-C port | ~$10 |

The Wi-Fi-only DevKitC row is the cheapest working setup: the `s3-eth` image notices there is no
W5500 (`no ethernet (...), wifi only` in the log) and Wi-Fi *becomes* the primary network. A Wi-Fi
SSID is mandatory there (console prompt or baked `CONFIG_WYZE_WIFI_SSID`). Make sure the dongle gets
5 V on the native "USB" port — the DevKitC-1 doesn't necessarily feed VBUS out of that port when
powered from the "UART" port; a powered OTG hub/Y-cable is the safe option.

**You need a Gateway:** download and run [`HclX/wyzesense2mqtt-rs`](https://github.com/HclX/wyzesense2mqtt-rs)
on any always-on machine (docs and setup in that repo). This firmware connects to its `/ws/bridge`
WebSocket endpoint and authenticates with the gateway's `bridge.auth_token`.

## Network behavior

- **Ethernet is primary.** Once it has an IP (not merely a link — a cable into a dead port or a
  switch without DHCP doesn't count), Wi-Fi shuts off (`wifi stopped (ethernet is primary)`).
- **Wi-Fi is a fallback** (when an SSID is configured): at boot it starts if Ethernet has no IP
  within 8 s, starts ~1 s after the link drops (or after Ethernet loses its DHCP address), retries
  every 5 s, and stops when Ethernet has an IP again. Never both in use at once. Boards without Ethernet hardware go straight to Wi-Fi.
- The WebSocket stays open only while the dongle is plugged in **and** a network is up. Switching
  between Ethernet and Wi-Fi, or replugging the dongle, recycles the gateway session. A wedged
  WebSocket client is detected and rebuilt (~2 min watchdog).
- `/status` reports `network` (`ethernet`/`wifi`/`none`) and the IP of the interface in use.

## Quick start (ESP32-P4)

1. **Buy the board** ([Amazon](https://www.amazon.com/dp/B0GFJQSN9B)) and assemble; PoE/PoE-splitter
   for power is optional (USB-C 5V also works).
2. **Download a release binary** from [GitHub Releases](https://github.com/rainyvalley/wyze-esp-bridge/releases)
   (or build from source, below). Pick the image by your chip revision — see [Firmware variants](#firmware-variants).
3. **Flash** over USB-C (dongle can stay plugged in; esptool auto-resets into download mode):
   ```bash
   pipx run esptool --chip esp32p4 -p /dev/ttyACM0 write_flash 0x0 wyze-esp-bridge-p4-rev1-merged.bin
   ```
   (esptool v5 renamed the subcommand to `write-flash`.)
4. **Configure on first boot**: open the console (`pipx run --spec pyserial pyserial-miniterm /dev/ttyACM0 115200`),
   and press **Enter** within 3 s of the banner to enter:
   - your gateway URI (e.g. `ws://192.168.1.50:8080/ws/bridge`)
   - your bridge token, matching the gateway's `bridge.auth_token` (masked input; Enter keeps the currently stored value)
   - Wi-Fi SSID and password (leave SSID blank for pure-Ethernet operation)

   At every prompt, Enter keeps the value in brackets and `-` clears it (e.g. `-` at the SSID
   prompt turns Wi-Fi off). A prompt left unanswered for 60 s cancels setup without saving, and the
   board boots normally.

   Values are stored in NVS and survive reboots and OTA updates. If you bake your values in at build
   time instead (`sdkconfig.local.defaults`, see [Build from source](#build-from-source)), just let
   the 3 s prompt time out.
5. The board boots into `waiting for Wyze dongle`; plug the dongle into the USB-A port (sold with
   your Wyze system, the little USB-A stick), and you'll see:
   ```
   wyze-esp-bridge 2.4.1 (esp32p4-eth)
   ...
   network up (wifi), IP 192.168.x.x     <- if no ethernet cable
   Wyze dongle up, connecting to gateway
   gateway connected
   ```
   (With no ethernet, Wi-Fi comes up ~5–10 s in; the dongle enumerates within ~16 s — a single
   automatic replug emulation may happen at ~10 s first. Be patient once; only replug by hand if
   it hasn't come up after ~30 s.)
6. **Pair sensors** on the gateway's own dashboard (`http://<gateway>:8080`), same as you would with
   a USB-attached dongle. The bridge presents itself to the gateway under `device=esp32p4-eth`.

### Wi-Fi fallback (optional)

The setup prompt asks for a Wi-Fi SSID and password at the same time as the gateway settings:

- **SSID + password stored → the board uses Ethernet when the cable is in, and Wi-Fi when it's out.**
- **Leave SSID blank (or enter `-` to clear a stored one) → Wi-Fi stays off entirely** (pure-Ethernet
  board, like before v2.2).
- The Wi-Fi runs on the board's ESP32-C6 co-processor over SDIO (ESP-Hosted). A factory C6 firmware
  prints `esp-hosted fw versions: host=3.x coprocessor=0.0.0`+`major version mismatch` at boot —
  harmless (association + data path work); updating the C6 firmware via esp-usb/esp-hosted OTA is
  on the roadmap. Currently 2.4 GHz only (C6 limit).
- Wi-Fi connect failures retry every 5 s; association works with WPA1/WPA2/WPA3-mixed APs, and with
  open APs only when no password is set (with a password set, an open AP using the same SSID is
  refused).

A useful debugging window between resets: board `/status` over Wi-Fi can be flaky in dual-homed
LANs (some APs isolate clients); the **gateway's dashboard is the source of truth** for whether a
bridge session is live (`/api/dongles` shows the session + its sensors).

## Firmware variants

ESP-IDF treats ESP32-P4 chip revisions **v3.x** and **v0.x/v1.x** as separate hardware. Check yours:

```bash
pipx run esptool --chip esp32p4 -p /dev/ttyACM0 chip_id
```

- Chip revision `v3.x` → use `wyze-esp-bridge-p4-*.bin`
- Chip revision `v0.x`/`v1.x` (e.g. `v1.3`) → use `wyze-esp-bridge-p4-rev1-*.bin`

Current `PARTITION` is printed in `/status`; the board has 16 MB flash and two 3 MB OTA slots. OTA
rollback is armed: a new image is on trial until its WebSocket connection to the gateway comes up,
and any reset before that (crash, watchdog, `/reboot`, saving settings on the console) rolls back to
the previous slot. If the gateway has not connected 5 minutes after the network came up (gateway
down or misconfigured, dongle unplugged), the image is kept anyway, so a gateway problem never
causes a rollback. An image that never gets an IP is never kept. While an image is on trial,
`POST /ota` answers `503` (ESP-IDF cannot start another update before the running one is confirmed).

### OTA updates (once the bridge is online)

Updates go over the network to `POST /ota`; no USB cable needed after the first flash.

**Your settings must be in NVS first.** Release images carry placeholder settings; the gateway URI,
token and Wi-Fi settings you entered on the console live in NVS and survive updates. If you instead
built your own image with `sdkconfig.local.defaults`, boot a 2.4.1 or later build of your own once
before installing a release image: it copies its built-in settings into NVS (the log says `saved the
built-in gateway/Wi-Fi settings to NVS`). Otherwise the release image boots with the placeholder
gateway, never connects, and is rolled back only if you reboot it within 5 minutes.

1. **Pick the right image.** Use the same variant you flashed first (see
   [Firmware variants](#firmware-variants)): `p4` for chip v3.x, `p4-rev1` for v0.x/v1.x, `s3-eth`
   for the S3 board. A wrong-variant image is rejected by the bridge (`500`) and nothing changes.

2. **Download it and check it.** From the [latest release](https://github.com/rainyvalley/wyze-esp-bridge/releases/latest),
   grab the `-ota.bin` for your variant and `SHA256SUMS`:

   ```bash
   V=p4-rev1   # or p4 / s3-eth
   gh release download -R rainyvalley/wyze-esp-bridge -p "wyze-esp-bridge-$V-ota.bin" -p SHA256SUMS
   sha256sum --check --ignore-missing SHA256SUMS
   ```

   (Without `gh`: download the same two files from the release page in a browser.)

3. **Note what's running now,** so you can tell the update took:

   ```bash
   curl -s http://<board-ip>/status   # "version", "built", "partition"
   ```

4. **Upload.** Use the bridge token you set on the console (leave the header out if none is set):

   ```bash
   curl --fail-with-body -H "Authorization: Bearer <token>" \
        --data-binary @wyze-esp-bridge-$V-ota.bin http://<board-ip>/ota
   ```

   Takes 10–30 s and answers `ok, rebooting`. The bridge restarts into the other OTA slot.

5. **Check it came back.** After ~20 s (P4: the dongle takes ~16 s to enumerate):

   ```bash
   curl -s http://<board-ip>/status   # new "version", the other "partition", "gateway_connected": true
   ```

   The new image is **on trial** until it connects to the gateway (or 5 minutes pass with the
   network up). `GET /log` shows `new firmware marked valid` once it is kept. Don't reboot or
   power-cycle it before then: a reset during the trial rolls back to the previous version.

| Response | Meaning |
|---|---|
| `ok, rebooting` | Image written and verified; the bridge restarts into it |
| `401 bad or missing token` | Wrong or missing `Authorization: Bearer` token |
| `503 … still on trial` | The current image isn't confirmed yet; wait for the gateway to connect (≤5 min) and retry |
| `400 missing or oversized image` | Empty upload or wrong file (use `-ota.bin`, not `-merged.bin`) |
| `500 <error>` | Image rejected, e.g. wrong chip variant or corrupt download; the running firmware is untouched |

If the bridge comes back on the old version, the new image was rolled back: check `GET /log`.

HTTP endpoints: `GET /status` (JSON), `GET /log` (this boot's ring buffer), `POST /ota` (token),
`POST /reboot` (token). Token can also be `?token=` query-param (percent-encoded).

`/status` and `/log` need no token. Values of `token=` are masked in the log. **While no token is
set, `/ota` and `/reboot` are open to anyone on the network**: set a token on the console, or build
with `CONFIG_WYZE_REQUIRE_TOKEN=y` to refuse them until one is set.

## Build from source

All variants at once (Docker, no IDF install needed):

```bash
docker run --rm -v "$PWD":/project -w /project espressif/idf:release-v5.5 ./build.sh
```

Output lands in `out/`: `<name>-merged.bin` (flash at 0x0) and `<name>-ota.bin` (POST to `/ota`).

Single variant (see `build.sh` for the pattern):

```bash
docker run --rm -v "$PWD":/project -w /project espressif/idf:release-v5.5 \
  idf.py -B build-p4-rev1 -D SDKCONFIG=build-p4-rev1/sdkconfig \
  -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.p4-rev1" \
  set-target esp32p4 reconfigure build
```

Every release binary on this repo is built by CI with `./build.sh` (all variants) in the same
container image, pinned by digest in `.github/workflows/release.yml`; releases include `SHA256SUMS`.

Site-specific values (gateway URI, token, Wi-Fi credentials) can be baked in through a git-ignored
`sdkconfig.local.defaults`, which `build.sh` appends when present. Images built that way contain
those secrets in plain text: never share or upload them.

Component versions are locked per target in `dependencies.lock.esp32p4` and
`dependencies.lock.esp32s3`.

## Protocol notes

- Dongle → gateway: HID input report `[len][data…]`, forward `data[0..len]` as one binary WS frame.
- Gateway → dongle: one binary WS frame = one protocol packet, sent as HID
  `SET_REPORT` (Output, report ID 0) — the dongle has no interrupt OUT endpoint. A packet larger
  than the dongle's output report (read from its HID report descriptor at plug-in and logged as
  `dongle output report: N bytes`; 64 if the descriptor can't be read) is dropped with a log line.
- Auth: `?token=<bridge token>` query parameter plus `device=<BOARD_NAME>`, per the gateway's
  [`multi_dongle_design.md`](https://github.com/HclX/wyzesense2mqtt-rs/blob/main/docs/multi_dongle_design.md).
- The WebSocket stays open only while the dongle is plugged in and the network is up, so the gateway
  sees a proper disconnect when the dongle is removed.

## Troubleshooting

- **`dongle:false` in `/status` but the gateway shows the bridge connected** — trust the gateway
  (`/api/dongles`); the bridge's own flags can be stale.
- **`HUB: Root port reset failed` at boot, then dongle enumerates ~16 s in** — normal: the boot
  settle bounce plus a one-shot replug emulation (8 s after power-on) recovers it. If the dongle
  never comes up, unplug/replug it once.
- **`StaDisconnected reason=211`** — the Wi-Fi scan threshold filtered your AP (older builds);
  v2.2.2+ uses the weakest PSK threshold (WPA) when a password is set, open otherwise. Reason 202 =
  wrong password.
- **`esp-hosted fw versions ... major version mismatch`** — the factory C6 co-processor firmware;
  benign, Wi-Fi still works.
- **Boot-loop on older P4 revisions with the dongle pre-powered** — fixed since v2.0.8
  (`root_port_unpowered` at install + power bounce before hub events). Update past that version.
- **`GET /log` shows only this boot** — it's a 48 KB RAM ring; there is no persistent log.

## Acknowledgments

- [`HclX/wyzesense2mqtt-rs`](https://github.com/HclX/wyzesense2mqtt-rs) — the gateway and the
  `/ws/bridge` protocol this firmware implements; this project started as its companion device firmware.
- [`raetha/wyzesense2mqtt`](https://github.com/raetha/wyzesense2mqtt) — original Python gateway,
  source of much of the underlying protocol knowledge.
- `HclX/WyzeSensePy` / community reverse-engineering of the Wyze Sense dongle protocol.

## Enclosure

`case/wyze_esp_bridge_p4_case.scad` — parametric OpenSCAD case for the P4 board (60 × 74.3 × 27.1 mm,
snap-fit or M2.5 screws). Previews in `case/*.png`. See comments in the `.scad` file for print settings
and `MEASURE`-marked values. `case/wyze_esp_bridge_case.scad` is for an earlier (obsolete) S3 build.

## License

[MIT](LICENSE)