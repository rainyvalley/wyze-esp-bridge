# wyze-esp-bridge

ESP32 USB host relay for [wyzesense2mqtt-rs](https://github.com/HclX/wyzesense2mqtt-rs): plug the
Wyze Sense Bridge dongle (USB `1a86:e024`) into the ESP32's USB host port, and the sensors end up on
your MQTT broker via the gateway — with the ESP32 on Ethernet *or Wi-Fi*, anywhere in the house, not
tethered to a USB port on the gateway machine. This project is a hardware counterpart to the
gateway's desktop `dongle_bridge` binary: it implements the same `/ws/bridge` wire protocol, but
the "cable" is a WebSocket.

A [Waveshare ESP32-P4-WIFI6-POE-ETH](https://www.waveshare.com/esp32-p4-wifi6-poe-eth.htm) board is the
recommended host ([Amazon US](https://www.amazon.com/dp/B0GFJQSN9B), [Waveshare wiki](https://www.waveshare.com/wiki/ESP32-P4-_WIFI6-POE-ETH)):
its USB-A port speaks the USB 2.0 host role the dongle needs, its internal Ethernet keeps the link
wired, and its ESP32-C6 co-processor provides Wi-Fi 6 as an automatic fallback when the cable is
out. A [Waveshare ESP32-S3-ETH](https://www.waveshare.com/esp32-s3-eth.htm) build is also provided
(no Wi-Fi fallback there — the S3 board has no co-processor).

| Build | Board | Ethernet | Wi-Fi fallback | Dongle port | Console |
|---|---|---|---|---|---|
| `p4` / `p4-rev1` | Waveshare **ESP32-P4-WIFI6-POE-ETH** (recommended) | Internal EMAC + IP101 | yes (ESP32-C6 over SDIO) | **USB-A** | USB-C (CH343) |
| `s3-eth` | Waveshare ESP32-S3-ETH (PoE) | W5500 over SPI | no | USB-C + OTG adapter | Header GPIO43/44 |

**You need a Gateway:** download and run [`HclX/wyzesense2mqtt-rs`](https://github.com/HclX/wyzesense2mqtt-rs)
on any always-on machine (docs and setup in that repo). This firmware connects to its `/ws/bridge`
WebSocket endpoint and authenticates with the gateway's `bridge.auth_token`.

## Network behavior

- **Ethernet is primary.** If a link is present at boot or comes up later, Wi-Fi shuts off
  (`wifi fallback stopped (ethernet is primary)`).
- **Wi-Fi is a fallback**, used only while the cable is unplugged or the link is down. It connects
  5 s after boot when no link, retries forever with a backoff, and shuts off the moment Ethernet
  returns. Never both at once.
- The WebSocket stays open only while the dongle is plugged in **and** any network is up; a wedged
  WebSocket client is detected and rebuilt (~2 min watchdog).

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
   
   Values are stored in NVS and survive reboots and OTA updates. If you leave the defaults baked into
   `sdkconfig.defaults` (edit + rebuild yourself), setup is skipped entirely.
5. The board boots into `waiting for Wyze dongle`; plug the dongle into the USB-A port (sold with
   your Wyze system, the little USB-A stick), and you'll see:
   ```
   wyze-esp-bridge 2.2.4 (esp32p4-eth)
   ...
   network up (wifi fallback), IP 192.168.x.x     <- if no ethernet cable
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
- **Leave SSID blank → Wi-Fi stays off entirely** (pure-Ethernet board, like before v2.2).
- The Wi-Fi runs on the board's ESP32-C6 co-processor over SDIO (ESP-Hosted). A factory C6 firmware
  prints `esp-hosted fw versions: host=3.x coprocessor=0.0.0`+`major version mismatch` at boot —
  harmless (association + data path work); updating the C6 firmware via esp-usb/esp-hosted OTA is
  on the roadmap. Currently 2.4 GHz only (C6 limit).
- Wi-Fi connect failures retry every 5 s; association works with WPA1/WPA2/WPA3-mixed and open APs.

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
rollback is armed: a new image only sticks if it reaches the network (gets an IP); otherwise the next
reset rolls back to the previous slot.

### OTA updates (once the bridge is online)

```bash
python3 - <<'EOF'
import urllib.request
token = "CHANGE_ME"  # your bridge token
with open("wyze-esp-bridge-p4-rev1-ota.bin","rb") as f: data = f.read()
req = urllib.request.Request("http://<board-ip>/ota", data=data, method="POST",
    headers={"Authorization": f"Bearer {token}"})
print(urllib.request.urlopen(req, timeout=180).read().decode())
EOF
```

HTTP endpoints: `GET /status` (JSON), `GET /log` (this boot's ring buffer), `POST /ota` (token),
`POST /reboot` (token). Token can also be `?token=` query-param.

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

Every release binary on this repo is built by CI from that exact command.

## Protocol notes

- Dongle → gateway: HID input report `[len][data…]`, forward `data[0..len]` as one binary WS frame.
- Gateway → dongle: one binary WS frame = one protocol packet, sent as HID
  `SET_REPORT` (Output, report ID 0) — the dongle has no interrupt OUT endpoint.
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
  v2.2.2+ uses an open scan threshold. Reason 202 = wrong password.
- **`esp-hosted fw versions ... major version mismatch`** — the factory C6 co-processor firmware;
  benign, Wi-Fi still works.
- **Boot-loop on older P4 revisions with the dongle pre-powered** — fixed since v2.0.8
  (`root_port_unpowered` at install + power bounce before hub events). Update past that version.
- **`GET /log` shows only this boot** — it's a 16 KB RAM ring; there is no persistent log.

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