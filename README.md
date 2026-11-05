# wyze-esp-bridge

Firmware for an ESP32 board that carries a Wyze Sense Bridge dongle (USB `1a86:e024`) over the
network. The board relays sensor data to a [wyzesense2mqtt-rs](https://github.com/HclX/wyzesense2mqtt-rs)
gateway over Ethernet or Wi-Fi, so the dongle no longer has to sit next to the gateway.

**You need a gateway.** Run [`HclX/wyzesense2mqtt-rs`](https://github.com/HclX/wyzesense2mqtt-rs) on a
machine that stays on. The board connects to its `/ws/bridge` address with the gateway's
`bridge.auth_token`.

## Boards

| Build | Board | Ethernet | Wi-Fi | Dongle goes in | Setup cable | Price |
|---|---|---|---|---|---|---|
| `p4` or `p4-rev1` | Waveshare [ESP32-P4-WIFI6-POE-ETH](https://www.waveshare.com/esp32-p4-wifi6-poe-eth.htm) ([Amazon](https://www.amazon.com/dp/B0GFJQSN9B)) | yes | backup | **USB-A** port | USB-C | about $25 |
| `s3-eth` | Waveshare [ESP32-S3-ETH](https://www.waveshare.com/esp32-s3-eth.htm) ([Amazon](https://a.co/d/0dSdbcFS)) | yes | backup | USB-C port, with an OTG adapter | header pins GPIO43 and GPIO44 | about $30 |
| `s3-eth` | ESP32-S3-DevKitC-1 N16R8 ([Amazon](https://a.co/d/04rXdiuX)) | no | **only network** | "USB" port, with an OTG adapter | "UART" USB-C port | about $10 |

The P4 board is the safest choice. On the DevKitC-1, Wi-Fi is the only network (the log says
`no ethernet (...), wifi only`), so set a Wi-Fi name. Its "USB" port must supply 5 V to the dongle; if
the board is powered from "UART", use a powered OTG hub.

## How it connects

- **Ethernet first.** It counts once it has an IP address, not just a link.
- **Wi-Fi is the backup** when a name is set: starts 8 s after boot without Ethernet, ~1 s after the
  link drops, retries every 5 s, and stops when Ethernet gets an address. Never both at once.
- The gateway link is open only while the dongle is plugged in and a network is up; unplugging the
  dongle or switching networks ends the session. No gateway for ~2 min restarts the client.
- Retries wait 5–10 s, randomized, so many boards do not retry in step.
- **Alarms wait for the gateway.** Up to 32 alarm/leak reports are held in RAM while the gateway is
  unreachable, replayed oldest-first 10 s after it reconnects, keeping the dongle's timestamps.
  Reports older than 10 min are dropped. `/status` shows `alarms_held`, `alarms_replayed` and
  `alarms_lost`; `CONFIG_WYZE_ALARM_REPLAY_DELAY_S` and `CONFIG_WYZE_ALARM_MAX_AGE_S` tune it.
- The board answers as `wyze-bridge-xxxx.local` (`xxxx` = last two MAC bytes, also in the boot log).
- With no network for 3 minutes (or none configured at all), it opens a setup Wi-Fi network: see
  [Web page and setup](#web-page-and-setup).

Wi-Fi notes: the P4's radio is the on-board ESP32-C6 (2.4 GHz only). WPA, WPA2 and WPA3-mixed are
accepted; with a password set, an open network using the same name is refused. If `/status` is
unreachable over Wi-Fi, the router may isolate clients; the gateway's `/api/dongles` is the source of
truth.

## Quick start (ESP32-P4)

1. **Power** the board with USB-C (5 V) or PoE.
2. **Download** a release file from [GitHub Releases](https://github.com/rainyvalley/wyze-esp-bridge/releases)
   (pick the file for your chip, see [Firmware variants](#firmware-variants)) or
   [build it yourself](#build-from-source).
3. **Flash** over USB-C; the dongle can stay plugged in.
   ```bash
   pipx run esptool --chip esp32p4 -p <serial-port> write_flash 0x0 wyze-esp-bridge-p4-rev1-merged.bin
   ```
   (esptool 5: `write-flash`.)
4. **Set it up** on the serial console: press **Enter** within 3 s of the banner.
   ```bash
   pipx run --spec pyserial pyserial-miniterm <serial-port> 115200
   ```
   - **Gateway URI**: e.g. `ws://<gateway-ip>:8080/ws/bridge`, without a token.
   - **Bridge token**: the gateway's `bridge.auth_token`.
   - **Wi-Fi name and password**: leave the name empty for Ethernet only.
   - **Setup page password**: for the board's `/setup` page, not the gateway (8–63 characters;
     `-` restores `wyzebridge`).

   Enter keeps the bracketed value, `-` clears it. No answer for 60 s cancels setup without saving.
   Settings survive restarts and updates. If your image already has them baked in, let the 3 s run out.
5. **Plug in the dongle.** The log shows `waiting for Wyze dongle`, then `network up (wifi), IP ...`
   (only without Ethernet), `Wyze dongle up, connecting to gateway` and `gateway connected`. Allow
   30 s; the board may replug the dongle once itself. On an Ethernet board, a cable may crowd the
   USB-A port; a short extension cable fixes that.
6. **Pair sensors** on the gateway's page (port 8080). The board appears as `device=esp32p4-eth`
   (or `esp32s3-eth`).

## Firmware variants

The P4 has two hardware revisions needing different files; check yours with
`pipx run esptool --chip esp32p4 -p <serial-port> chip_id`.

| Chip revision | File |
|---|---|
| v3.x | `wyze-esp-bridge-p4-*.bin` |
| v0.x / v1.x | `wyze-esp-bridge-p4-rev1-*.bin` |

S3 boards use `wyze-esp-bridge-s3-eth-*.bin`. The board has 16 MB of flash and two 3 MB firmware
slots; `/status` shows the running one (`partition`).

**New firmware is on trial.** It is kept once it connects to the gateway; any restart before that
(including a crash, `/reboot` or saving settings) rolls back. If the gateway never connects, it is
kept 5 minutes after the network came up; an image that never gets an IP is never kept. While on
trial, `POST /ota` answers `503`.

### Update over the network

Use `POST /ota`; no USB cable is needed after the first flash.

**Settings must be on the board first.** Release files carry placeholder settings. Values entered at
setup are stored on the board. If you built an image with settings inside, run it once before
installing a release file (its first boot logs `saved the built-in gateway/Wi-Fi settings to NVS`), or
the release file starts with the placeholder gateway.

1. **Pick the same variant** you flashed (`p4`, `p4-rev1` or `s3-eth`). A wrong variant gets `500`.
2. **Download and verify** the `-ota.bin` and `SHA256SUMS` from the
   [latest release](https://github.com/rainyvalley/wyze-esp-bridge/releases/latest):
   ```bash
   V=p4-rev1   # or p4 / s3-eth
   gh release download -R rainyvalley/wyze-esp-bridge -p "wyze-esp-bridge-$V-ota.bin" -p SHA256SUMS
   sha256sum --check --ignore-missing SHA256SUMS
   ```
   Without `gh`, download both in a browser. The checksum catches damage, not tampering.
3. **Note the current version:** `curl -s http://<board-ip>/status` (`version`, `built`, `partition`).
4. **Upload** with the token (omit the header if none is set):
   ```bash
   curl --fail-with-body -H "Authorization: Bearer <token>" \
        --data-binary @wyze-esp-bridge-$V-ota.bin http://<board-ip>/ota
   ```
   Takes 10–30 s and answers `ok, rebooting`.
5. **Check it came back** after ~20 s: `/status` should show the new `version`, the other `partition`
   and `"gateway_connected": true`. The setup page log shows `new firmware marked valid` once it is
   kept; do not restart before that.

| Response | Meaning |
|---|---|
| `ok, rebooting` | Written and verified; restarting. |
| `401 bad or missing token` | Wrong or missing token. |
| `429 too many wrong tokens` | Wait the seconds in `Retry-After`. |
| `503 ... still on trial` | Wait until the gateway connects (≤5 min). |
| `400 missing or oversized image` | Use `-ota.bin`, not `-merged.bin`. |
| `500 <error>` | Wrong variant or damaged file; nothing changed. |

If the old version comes back, it was rolled back: read the setup page log.

## Web page and setup

`http://wyze-bridge-xxxx.local/` (or the IP) shows network, gateway, dongle, held alarms and the
sensors heard since boot (type, state, battery, signal, last report). The sensor list starts empty
after a restart.

`/setup` edits the console settings (gateway URI, token, Wi-Fi, setup password) and shows the log. It
is locked behind the **setup password**, `wyzebridge` until changed, so change it: the default is
published here. The bridge token (the gateway secret) also unlocks the page, so a forgotten password
is recoverable with it. Blank fields keep stored values, `-` clears them (`-` restores the default
password). Saving restarts the board. The password is stored on the board, never shown on a page or in
`/status`/`/log`, and must be 8–63 printable characters without a leading or trailing space. Wrong
passwords share the token lockout (5 tries, then 30 s doubling to 15 min). Cross-site form posts are
refused, and saving is blocked while new firmware is on trial. Change the default with
`CONFIG_WYZE_SETUP_PASSWORD`; `CONFIG_WYZE_REQUIRE_TOKEN` covers `/ota` and `/reboot` only.

| Address | What it does | Credential |
|---|---|---|
| `GET /` | Status page (network, gateway, dongle, alarms, sensors). It used to return the `/status` JSON. | no |
| `GET /status`, `GET /sensors` | The same state and sensors as JSON. | no |
| `GET /setup`, `POST /setup` | Settings page and log; saving restarts the board. | setup password |
| `GET /log` | Log as plain text (see [The log](#the-log)). | setup password or token |
| `POST /ota`, `POST /reboot` | Install firmware; restart. | token |

The token goes in `Authorization: Bearer` or `?token=` (URL-encoded). The setup password goes in
`X-Setup-Password`; the page does that itself.

**Setup Wi-Fi network.** With no working network the board opens `wyze-bridge-xxxx`; join it and open
<http://192.168.4.1/setup>. It opens at once on a board with no Ethernet and no Wi-Fi name, otherwise
after 3 minutes without any IP (at boot or later: no cable or DHCP, wrong password, router gone). It
stays a minute after a network returns, then closes; with no Wi-Fi name it also closes after 30
minutes and stays closed until a network has been up again. The network is open by default (set
`CONFIG_WYZE_SETUP_AP_PASSWORD` for WPA2), but the form still needs the setup password. On the P4 it
is **experimental and off by default** (`CONFIG_WYZE_SETUP_AP_HOSTED=y`); boards without the flag use
`/setup` over Ethernet. The Wi-Fi radio is started for the setup network even with no name stored.

## The log

The log is at the bottom of `/setup` (Refresh, or every 5 s) and behind the setup password because it
names sensors and addresses. It shows the end of the previous run, `--- restart ---`, then this run,
newest last; times are seconds since boot. For scripts:

```bash
curl -H "X-Setup-Password: <setup password>" http://<board-ip>/log
curl -H "Authorization: Bearer <bridge token>" http://<board-ip>/log
```

- **48 KB RAM buffer**, oldest text overwritten when full.
- **Repeats are collapsed** (e.g. a retry loop): three lines in full, then one `N repeats of "..."`
  note per 30 minutes. Console output is collapsed too.
- **Survives restarts.** Up to 32 KB is saved to flash every 20 minutes if it changed, on restarts it
  initiates itself (saving settings, OTA, `/reboot`), and a few seconds after a warning or error
  (bounded, so a fault cannot wear the flash). The first boot line names the reset reason. A power cut
  loses what came after the last snapshot. It lives in the last 64 KB of flash, in two alternating
  slots outside every partition, so updates and the partition table are untouched; it is disabled if
  the area overlaps a partition or the flash is smaller.
  `CONFIG_WYZE_LOG_SNAPSHOT_MINUTES` tunes it; `CONFIG_WYZE_LOG_PERSIST=n` turns it off.
- **Secrets:** `token=` values are masked in the log and snapshots; no password is logged.

## Security

- Gateway and web traffic are plain `ws://`/`http://`; tokens and passwords cross the network in the
  clear. Use a trusted network.
- **While no token is set, `/ota` and `/reboot` are open to the LAN.** Set one, or build with
  `CONFIG_WYZE_REQUIRE_TOKEN=y`.
- `/`, `/status` and `/sensors` are open; `/log` and `/setup` need the setup password or token.
- **Wrong credentials are limited:** 5 in a row, then `429` for 30 s, doubling to 15 min
  (`CONFIG_WYZE_AUTH_MAX_FAILS`). The lock is shared, so a guesser can briefly block you. `/status`
  and `/log` also answer `429` above ~4 requests a second.
- Keep the token in the token field, not the gateway URI. The token, setup password and Wi-Fi
  password are stored unencrypted; anyone with the board and its serial port has full control.
- Firmware is not signed; any valid image with the right token is accepted.

## Build from source

```bash
docker run --rm -v "$PWD":/project -w /project espressif/idf:release-v5.5 ./build.sh
```

Outputs in `out/`: `<name>-merged.bin` (flash at 0x0) and `<name>-ota.bin` (send to `/ota`). For one
variant (see `build.sh`):

```bash
docker run --rm -v "$PWD":/project -w /project espressif/idf:release-v5.5 \
  idf.py -B build-p4-rev1 -D SDKCONFIG=build-p4-rev1/sdkconfig \
  -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.p4-rev1" \
  set-target esp32p4 reconfigure build
```

CI builds releases in a pinned container and includes `SHA256SUMS`.

`tests/host/` has host unit tests for the pure modules (packet scanner, alarm ring and sensor table,
log handling, `main/util.c`). They need no ESP-IDF or board; CI's `tests` job runs them first.

Site settings (gateway, token, Wi-Fi) can go in a git-ignored `sdkconfig.local.defaults`, which
`build.sh` appends. Such images contain the secrets in plain text: never share them. Library versions
are pinned in `dependencies.lock.esp32p4` and `dependencies.lock.esp32s3`. Maintainer notes:
[AGENTS.md](AGENTS.md).

## Troubleshooting

| Symptom | Cause | Fix |
|---|---|---|
| `/status` says `dongle: false`, gateway shows the bridge | The two disagree. | Trust the gateway's `/api/dongles`. |
| `HUB: Root port reset failed`, dongle appears ~16 s later | Normal P4 boot; the board replugs once. | Wait; replug by hand only if it never appears. |
| `wifi disconnected (reason 202)` | Wrong Wi-Fi password. | Run setup again. |
| reason `201` or `211` | Network not found. | Check the name and that it is 2.4 GHz. |
| `esp-hosted fw versions ... major version mismatch` | Older C6 factory firmware. | Nothing; Wi-Fi works. |
| `no gateway connection for 2 min, restarting the client` | Gateway unreachable. | Check the gateway, URI and token. |
| `/setup` or `/log` asks for a password | Both are locked. | `wyzebridge` until changed; the token works too. |
| Forgot the setup password | The token also unlocks it. | Use the token, set a new password, or `-` at the console prompt. |
| Setup Wi-Fi network did not appear | Needs 3 min without an IP; off by default on P4. | Look for `no network address`; build with `CONFIG_WYZE_SETUP_AP_HOSTED=y`. |
| Log misses the minutes before a restart | Snapshots every 20 min; a power cut loses the rest. | See [The log](#the-log). |
| An alarm from an outage never arrived | Only alarm/leak, ≤32, ≤10 min, RAM only. | See [How it connects](#how-it-connects). |

## Acknowledgments

- [`HclX/wyzesense2mqtt-rs`](https://github.com/HclX/wyzesense2mqtt-rs): the gateway and `/ws/bridge`
  protocol.
- [`raetha/wyzesense2mqtt`](https://github.com/raetha/wyzesense2mqtt) and `HclX/WyzeSensePy`: original
  gateway and protocol research.

## Enclosure

`case/wyze_esp_bridge_p4_case.scad` is a printable P4 case (60 × 74.3 × 27.1 mm, snap-fit or M2.5
screws); previews are `case/p4-preview-*.png`. `case/wyze_esp_bridge_case.scad` is the S3 case
(previews `case/s3-preview-*.png`). The `.scad` files explain print settings and `MEASURE` values.

## A note on AI assistance

Parts of this project (code, configuration, documentation) were written with generative AI and were
reviewed, tested, and corrected by a human before committing.

## License

[BSD 2-Clause](LICENSE)
