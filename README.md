# wyze-esp-bridge

Firmware for an ESP32 board that carries your Wyze Sense Bridge dongle over the network.

Plug the dongle (USB `1a86:e024`) into the board. The board sends the sensor data to your
[wyzesense2mqtt-rs](https://github.com/HclX/wyzesense2mqtt-rs) gateway over Ethernet or Wi-Fi.
The gateway then publishes it to your MQTT broker. The dongle no longer has to sit next to the gateway.

**You need a gateway.** Install and run [`HclX/wyzesense2mqtt-rs`](https://github.com/HclX/wyzesense2mqtt-rs)
on any machine that stays on. This firmware connects to its `/ws/bridge` address and signs in with the
gateway's `bridge.auth_token`.

## Boards

All boards run the same firmware. Pick one by how you want to connect.

| Build | Board | Ethernet | Wi-Fi | Dongle goes in | Setup cable | Price |
|---|---|---|---|---|---|---|
| `p4` or `p4-rev1` | Waveshare [ESP32-P4-WIFI6-POE-ETH](https://www.waveshare.com/esp32-p4-wifi6-poe-eth.htm) ([Amazon](https://www.amazon.com/dp/B0GFJQSN9B)) | yes | backup | **USB-A** port | USB-C | about $25 |
| `s3-eth` | Waveshare [ESP32-S3-ETH](https://www.waveshare.com/esp32-s3-eth.htm) ([Amazon](https://a.co/d/0dSdbcFS)) | yes | backup | USB-C port, with an OTG adapter | header pins GPIO43 and GPIO44 | about $30 |
| `s3-eth` | ESP32-S3-DevKitC-1 N16R8 ([Amazon](https://a.co/d/04rXdiuX)) | no | **only network** | "USB" port, with an OTG adapter | "UART" USB-C port | about $10 |

The P4 board is the one to buy if you are unsure.

On the DevKitC-1 the firmware logs `no ethernet (...), wifi only` and uses Wi-Fi as its main network.
You must set a Wi-Fi name there. The dongle needs 5 V from the "USB" port. That port may not supply
power when the board is powered from the "UART" port, so use a powered OTG hub if the dongle does not light up.

## How it connects

- **Ethernet comes first.** It counts once the board has an IP address. A cable in a dead port does not count.
- **Wi-Fi is the backup**, and only if you set a Wi-Fi name. At boot it starts if Ethernet has no address after 8 s.
  It starts about 1 s after Ethernet drops. It retries every 5 s. It stops when Ethernet has an address again.
- The board never uses both at once. Boards with no Ethernet go straight to Wi-Fi.
- The link to the gateway is open only while the dongle is plugged in and a network is up.
  Unplugging the dongle or switching between Ethernet and Wi-Fi ends the gateway session, and a new one starts.
- If the board has had no gateway connection for about 2 minutes, it restarts its gateway client. This also
  happens when the gateway is simply off.
- Between attempts the board waits 5 to 10 s, a random time each start, so many boards do not retry in step.
- `/status` shows which network is in use (`ethernet`, `wifi` or `none`) and its IP address.
- **Sensor alarms wait for the gateway.** The dongle keeps hearing sensors while the network or the gateway is
  down. The board holds up to 32 alarm/leak reports (heartbeats and climate readings repeat on their own and are
  not held) and sends them oldest-first once the gateway has been connected for 10 s; each keeps the dongle's own
  timestamp. Reports older than 10 min are dropped and a full buffer drops its oldest. `/status` shows
  `alarms_held`, `alarms_replayed` and `alarms_lost`. Held alarms live in RAM, so a restart discards them.
  Settings: `CONFIG_WYZE_ALARM_REPLAY_DELAY_S`, `CONFIG_WYZE_ALARM_MAX_AGE_S`.
- **The board has a name.** It answers as `wyze-bridge-xxxx.local` (`xxxx` is the last two bytes of its MAC, also
  in the boot log) and advertises an `_http._tcp` service, so you do not need its IP address.
- **No network at all:** if neither Ethernet nor Wi-Fi has an address for 3 minutes (or at once on a board with no
  Ethernet and no Wi-Fi name), the board opens its own setup Wi-Fi network. See
  [Web page and setup](#web-page-and-setup).

## Quick start (ESP32-P4)

1. **Get the board.** Power it with USB-C (5 V) or PoE.
2. **Download a release file** from [GitHub Releases](https://github.com/rainyvalley/wyze-esp-bridge/releases),
   or [build it yourself](#build-from-source). Choose the file for your chip, see [Firmware variants](#firmware-variants).
3. **Flash it** over USB-C. The dongle can stay plugged in.
   ```bash
   pipx run esptool --chip esp32p4 -p <serial-port> write_flash 0x0 wyze-esp-bridge-p4-rev1-merged.bin
   ```
   With esptool 5 the command is `write-flash`.
4. **Set it up.** Open the serial console and press **Enter** within 3 s of the banner.
   ```bash
   pipx run --spec pyserial pyserial-miniterm <serial-port> 115200
   ```
   You are asked for:
   - **Gateway URI**, for example `ws://<gateway-ip>:8080/ws/bridge`. Do not put a password or token in it.
   - **Bridge token**, the same as the gateway's `bridge.auth_token`. Typing is hidden.
   - **Wi-Fi name and password.** Leave the name empty if you only use Ethernet.
   - **Setup page password**, for the board's own `/setup` page (not the gateway). 8 to 63 characters;
     Enter keeps it and `-` puts the default `wyzebridge` back. See [Web page and setup](#web-page-and-setup).

   At each prompt, Enter keeps the value in brackets and `-` clears it. If you do not answer for
   60 s, setup is cancelled and nothing is saved. The board then starts normally.

   The values are saved on the board. They stay after a restart and after a firmware update.
   If you built your own image with the values inside (see [Build from source](#build-from-source)),
   skip setup and let the 3 s run out.
5. **Plug in the dongle.** The log shows `waiting for Wyze dongle`, then:
   ```
   network up (wifi), IP ...          <- only without an Ethernet cable
   Wyze dongle up, connecting to gateway
   gateway connected
   ```
   Give it up to 30 s. The dongle can take about 16 s to be found, and the board may unplug and replug
   it once on its own. Replug it by hand only if nothing happens after 30 s. On an Ethernet board, a
   cable can leave too little room beside the USB-A port; a short USB-A extension cable fixes that.
6. **Pair sensors** on the gateway's own web page (port 8080), as you would with a dongle on the gateway itself.
   The gateway sees this board as `device=esp32p4-eth` (or `esp32s3-eth`).

### Wi-Fi backup

- **Wi-Fi name and password saved:** Ethernet is used when the cable is in, Wi-Fi when it is out.
- **Wi-Fi name empty (or cleared with `-`):** Wi-Fi stays off.
- Wi-Fi on the P4 board is provided by a second chip (the ESP32-C6) on the same board. It works on 2.4 GHz only.
- Wi-Fi accepts WPA, WPA2 and WPA3-mixed networks. If a password is set, an open network with the same name is refused.
- A failed connection is retried every 5 s.

If you cannot open `/status` over Wi-Fi, your router may be blocking device-to-device traffic.
Check the gateway's web page instead (`/api/dongles` lists the bridge and its sensors).

## Firmware variants

The ESP32-P4 chip comes in two hardware revisions, and they need different files. Check yours:

```bash
pipx run esptool --chip esp32p4 -p <serial-port> chip_id
```

| Chip revision | Use |
|---|---|
| v3.x | `wyze-esp-bridge-p4-*.bin` |
| v0.x or v1.x (for example v1.3) | `wyze-esp-bridge-p4-rev1-*.bin` |

The ESP32-S3 board uses `wyze-esp-bridge-s3-eth-*.bin`.

The board has 16 MB of flash and two 3 MB firmware slots. `/status` shows which one is running (`partition`).

**A new firmware is on trial first.** It is kept once it connects to the gateway. Any restart before
that returns the board to the old firmware. This includes a crash, `/reboot` and saving settings on the console.
If the gateway has not connected 5 minutes after the network came up, the new firmware is kept anyway.
A firmware that never gets an IP address is never kept. While one is on trial, `POST /ota` answers `503`.

### Update over the network

Updates use `POST /ota`. You do not need the USB cable after the first flash.

**Your settings must be saved on the board first.** Release files contain placeholder settings. The gateway
address, token and Wi-Fi details you typed at setup are saved on the board and kept. If you built your own
image with settings inside, run that image once before you install a release file. On its first start it
saves those settings to the board (the log says `saved the built-in gateway/Wi-Fi settings to NVS`).
Without this, the release file starts with the placeholder gateway and never connects.

1. **Pick the right file.** Use the same variant you flashed first: `p4`, `p4-rev1` or `s3-eth`.
   The wrong variant is refused with `500` and nothing changes.

2. **Download and check it.** Get the `-ota.bin` for your variant and `SHA256SUMS` from the
   [latest release](https://github.com/rainyvalley/wyze-esp-bridge/releases/latest):

   ```bash
   V=p4-rev1   # or p4 / s3-eth
   gh release download -R rainyvalley/wyze-esp-bridge -p "wyze-esp-bridge-$V-ota.bin" -p SHA256SUMS
   sha256sum --check --ignore-missing SHA256SUMS
   ```

   Without `gh`, download both files from the release page in a browser. `SHA256SUMS` catches a damaged
   download. It does not prove who made the file.

3. **Note what runs now:**

   ```bash
   curl -s http://<board-ip>/status   # look at "version", "built", "partition"
   ```

4. **Upload.** Use the token you set at setup. Leave the header out if no token is set.

   ```bash
   curl --fail-with-body -H "Authorization: Bearer <token>" \
        --data-binary @wyze-esp-bridge-$V-ota.bin http://<board-ip>/ota
   ```

   It takes 10 to 30 s and answers `ok, rebooting`.

5. **Check it came back.** Wait about 20 s, then:

   ```bash
   curl -s http://<board-ip>/status   # new "version", the other "partition", "gateway_connected": true
   ```

   The log on the setup page shows `new firmware marked valid` once the new firmware is kept. Do not restart
   or unplug the board before that, or it goes back to the old firmware.

| You see | It means | Do this |
|---|---|---|
| `ok, rebooting` | The file was written and checked. | Wait for the restart. |
| `401 bad or missing token` | The token is wrong or missing. | Send `Authorization: Bearer <token>`. |
| `429 too many wrong tokens` | Too many wrong tokens in a row. | Wait the number of seconds in the `Retry-After` header. |
| `503 ... still on trial` | The running firmware is not confirmed yet. | Wait until the gateway connects (5 min at most), then try again. |
| `400 missing or oversized image` | The upload is empty or the wrong file. | Use the `-ota.bin` file, not `-merged.bin`. |
| `500 <error>` | The file was refused, for example wrong chip or damaged download. | Check the variant and the checksum. The running firmware is not changed. |

If the board comes back on the old version, the new firmware was rolled back. Read the log on the setup page.

### Web addresses on the board

| Address | What it does | Credential |
|---|---|---|
| `GET /` | The status page: network, gateway, dongle, held alarms and the sensors heard since boot. (It used to return the `/status` JSON.) | no |
| `GET /status` | The same state as JSON. | no |
| `GET /sensors` | The sensors heard since boot, as JSON. | no |
| `GET /setup` | The settings page and the log. | setup password |
| `GET /log` | The log as plain text (see [The log](#the-log)). | setup password or token |
| `POST /setup` | Saves the settings from the page and restarts the board. | setup password |
| `POST /ota` | Installs a firmware file. | token |
| `POST /reboot` | Restarts the board. | token |

The token goes in the `Authorization: Bearer` header. `?token=` in the address also works, with the
token written in URL form (for example `%2B` for `+`). A header keeps the token out of addresses and logs.
The setup password goes in the `X-Setup-Password` header (the page does that itself); the bridge token
works there too.

## Web page and setup

`http://wyze-bridge-xxxx.local/` (or the board's IP) shows network, gateway, dongle, held alarms, and a
table of the sensors the dongle has heard since boot: type, state, battery voltage, signal strength and
time of the last report. The table is built from the reports passing through the bridge, so a sensor
appears after its first report and the list starts empty after a restart.

**Settings page (`/setup`).** Edits the same settings as the console prompts: gateway URI, bridge token,
Wi-Fi name and password, and the setup password itself. It sits behind a **setup password**, `wyzebridge`
until you change it: open the page, enter the password, and the form unlocks (nothing is shown before
that). A blank field keeps the stored value and `-` clears it; for the setup password `-` puts the
default back. Saving restarts the board. The board's [log](#the-log) is shown on the same page once it is
unlocked.

- **Changing it:** the "New setup password" box in the form, or the last console prompt. 8 to 63
  printable ASCII characters, without a leading or trailing space. The page warns while the default is
  still in use; since the default is published here, change it. It is stored on the board, survives
  restarts and updates, and is never shown on any page or in `/status` or `/log`.
- **It is not the bridge token.** The bridge token is the secret shared with the gateway. The two are
  separate, but the bridge token also unlocks the page, so if you forget the setup password you can get
  in with the token and set a new one (or enter `-` at the console prompt).
- **Guessing is limited.** Wrong passwords count like wrong tokens: after 5 in a row the page answers
  `429` for 30 s, doubling with each further wrong try up to 15 min. The same lock is shared with `/ota`
  and `/reboot`.
- **Plain HTTP:** the password crosses your network unencrypted, like the OTA token does.
- Requests a browser marks as cross-site are refused, so a web page you happen to visit cannot
  reconfigure the bridge. Like `/ota`, saving is refused while a fresh firmware image is still on trial.
  Builds can change the default with `CONFIG_WYZE_SETUP_PASSWORD`. `CONFIG_WYZE_REQUIRE_TOKEN` applies to
  `/ota` and `/reboot` only; the setup page is always behind its password.

**Setup Wi-Fi network.** When a board has no working network it opens a Wi-Fi network named
`wyze-bridge-xxxx` (the same `xxxx` as the mDNS name). Join it and open <http://192.168.4.1/setup> (the
setup password unlocks the form). It opens

- at once on a board with no Ethernet and no Wi-Fi name (the DevKitC-1 on first start), and
- on any other board, including the S3-ETH PoE one, whenever neither Ethernet nor Wi-Fi has had an IP
  address for 3 minutes, at boot or later: no cable or DHCP, a wrong Wi-Fi password, a router that is
  gone. (A working Wi-Fi backup catches a pulled cable first, so the network only appears if that fails
  too, or if no name is stored.) A router restart that long opens it as well. The Wi-Fi radio is started
  for it even when no Wi-Fi name is stored.

Once Ethernet or Wi-Fi gets an address the setup network stays up for another minute (so a form in
progress finishes) and then closes. With no Wi-Fi name to fall back to, it also closes 30 minutes after
opening and then stays closed until a network has been up again (on a board that never had one, until a
power cycle), so a dead cable cannot keep it cycling. The network is open by default, so anyone in radio
range can join it, but not change anything without the setup password; set `CONFIG_WYZE_SETUP_AP_PASSWORD`
for a WPA2 network. On the ESP32-P4, whose Wi-Fi is the C6 co-processor, it is **experimental and off by
default** (build with `CONFIG_WYZE_SETUP_AP_HOSTED=y`); without it those boards use `/setup` over
Ethernet. With the flag, a P4 that has a stored Wi-Fi name but no working network adds the setup network
next to its Wi-Fi attempts; one with no name at all has to start the C6 radio from scratch to open it.

## The log

The log is at the bottom of the `/setup` page once you unlock it (Refresh, or tick "every 5 s"); it has
no page of its own, and it is behind the setup password because it names your sensors and addresses. It
is the end of the log from before the last restart, a `--- restart ---` line, then this run's log, newest
at the bottom. Times in it are seconds since that boot, not clock time.

For scripts, `GET /log` returns the same text as plain text, with either credential:

```bash
curl -H "X-Setup-Password: <setup password>" http://<board-ip>/log
curl -H "Authorization: Bearer <bridge token>" http://<board-ip>/log
```

- **Size:** this run's log is a 48 KB RAM buffer; the oldest text is overwritten when it fills.
- **Repeats are collapsed.** A line that repeats on a loop (a gateway that is down is retried every 5 s,
  and each attempt writes about seven lines) is written in full three times, then dropped, with one
  `N repeats of "..."` note per message every 30 minutes. Without this, a single outage fills the buffer
  in about 8 minutes and pushes everything useful out; with it, hours of outage cost a few KB. The same
  applies to the serial console. A message that stops for a minute and comes back is logged in full
  again.
- **It survives restarts.** The tail of the log (up to 32 KB) is saved to flash at most every 20 minutes,
  and only if the log changed, plus once on every restart the board starts itself (saving settings,
  update, `/reboot`) and a few seconds after a warning or error line (4 in a row, then one per 10
  minutes, so a fault cannot wear the flash). The first line of every boot names the reset reason
  (power-on, crash, watchdog, brownout...). A power cut or a crash loses what was logged since the last
  snapshot, which is why an incident log can end a few minutes early. History from several restarts is
  kept in the same 32 KB, newest last.
- **Where it lives:** the last 64 KB of the 16 MB flash, in two 32 KB slots written alternately so a power
  cut mid-write leaves the other one intact. No partition uses that area, so updates are unaffected and
  the partition table did not change. At boot the area is checked against the actual partition table and
  a smaller flash; if either looks wrong nothing is written. Tunable with
  `CONFIG_WYZE_LOG_SNAPSHOT_MINUTES`; `CONFIG_WYZE_LOG_PERSIST=n` turns it off.
- **Secrets:** values of `token=` are masked before they reach the log or the snapshots, and no password
  is ever logged.

## Security

- The board talks to the gateway with `ws://`, and its web address is plain `http://`. Nothing is encrypted.
  The token is sent in the clear. Use this on a network you trust.
- **While no token is set, anyone on the network can use `/ota` and `/reboot`.** Set a token at setup.
  Or build with `CONFIG_WYZE_REQUIRE_TOKEN=y`, which refuses both until a token is set.
- `/`, `/status` and `/sensors` need no token: they show the board's version, IP address, gateway address
  and sensors. `/log` needs the setup password (or the bridge token) because it names sensors and addresses.
- **The setup page has its own password** (`wyzebridge` until you change it); the bridge token unlocks it
  too. It is stored on the board and never shown on a page or in `/status` or `/log`.
- **Wrong credentials are limited.** After 5 wrong tokens or setup passwords in a row on `/ota`,
  `/reboot` or the setup page (setting `CONFIG_WYZE_AUTH_MAX_FAILS`), the board answers `429` for 30 s.
  Each further wrong try doubles the wait, up to 15 min. A correct token or password, or a restart,
  clears it. The lock is shared by all clients and all of those routes, so someone guessing can also
  block you for a while. `/status` and `/log` answer `429` if asked more than about 4 times a second.
- Put the token in the token field, not in the gateway URI. Only a `token=` in the URI is hidden in `/status` and `/log`.
- The token, setup password and Wi-Fi password are saved on the board without encryption. Anyone who
  holds the board and its serial port has full control of it.
- Firmware files are not signed. The board accepts any valid image sent with the right token.

## Build from source

Build every variant with Docker (no ESP-IDF install needed):

```bash
docker run --rm -v "$PWD":/project -w /project espressif/idf:release-v5.5 ./build.sh
```

The files appear in `out/`: `<name>-merged.bin` (flash at 0x0) and `<name>-ota.bin` (send to `/ota`).

For one variant (see `build.sh`):

```bash
docker run --rm -v "$PWD":/project -w /project espressif/idf:release-v5.5 \
  idf.py -B build-p4-rev1 -D SDKCONFIG=build-p4-rev1/sdkconfig \
  -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.p4-rev1" \
  set-target esp32p4 reconfigure build
```

Release files are built by CI with `./build.sh` in a pinned container image. Each release includes `SHA256SUMS`.

`tests/host/` has host unit tests for the pure helpers: the dongle packet scanner, the alarm ring and
sensor table, the log handling, and `main/util.c`. They need no ESP-IDF and no board; CI runs them
before every build (see the `tests` job in `.github/workflows/release.yml`).

To build in your own settings (gateway address, token, Wi-Fi), put them in a file named
`sdkconfig.local.defaults`. Git ignores it, and `build.sh` adds it when it exists. Such an image
contains those secrets as plain text, so never share it.

The versions of the libraries used are fixed in `dependencies.lock.esp32p4` and `dependencies.lock.esp32s3`.

Maintainer notes, including the data flow and known pitfalls, are in [AGENTS.md](AGENTS.md).

## Troubleshooting

| You see | It means | Do this |
|---|---|---|
| `/status` says `dongle` is false, but the gateway shows the bridge | The two views disagree. | Trust the gateway's `/api/dongles`. |
| `HUB: Root port reset failed` at boot, then the dongle appears after about 16 s | Normal on the P4 board. The board replugs the dongle itself once. | Wait. Replug by hand only if it never comes up. |
| `wifi disconnected (reason 202)` | Wrong Wi-Fi password. | Run setup again. |
| `wifi disconnected (reason 201)` or `211` | The Wi-Fi network was not found. | Check the name and that it is 2.4 GHz. |
| `esp-hosted fw versions ... major version mismatch` | The Wi-Fi chip on the P4 board has older factory firmware. | Nothing. Wi-Fi still works. |
| `no gateway connection for 2 min, restarting the client` | The gateway has been unreachable for 2 minutes. | Check that the gateway is running and the URI and token are right. |
| `/setup` or `/log` asks for a password | Both are behind the setup page password; the bridge token works too. | Use `wyzebridge` until you change it, or `curl -H "X-Setup-Password: <password>"` for `/log`. |
| Forgot the setup page password | The bridge token unlocks the page too. | Type the token in the password box and set a new password, or enter `-` at the console prompt. |
| The setup Wi-Fi network did not appear | It needs 3 minutes without any address, and on the P4 it is off by default. | Look for `no network address` in the log; build with `CONFIG_WYZE_SETUP_AP_HOSTED=y` on the P4. |
| The log is missing something from before a restart | The tail is saved only every 20 minutes and on restarts the board starts itself. | See [The log](#the-log); a power cut loses the last stretch. |
| An alarm from during an outage never reached the gateway | Only alarm and leak reports are held, at most 32 and for 10 minutes, and only in RAM. | See [How it connects](#how-it-connects); `/status` shows `alarms_held`, `alarms_replayed` and `alarms_lost`. |

## Acknowledgments

- [`HclX/wyzesense2mqtt-rs`](https://github.com/HclX/wyzesense2mqtt-rs) is the gateway and defines the
  `/ws/bridge` protocol this firmware uses.
- [`raetha/wyzesense2mqtt`](https://github.com/raetha/wyzesense2mqtt) is the original Python gateway
  and the source of much of what is known about the protocol.
- `HclX/WyzeSensePy` and the community work on the Wyze Sense dongle protocol.

## Enclosure

`case/wyze_esp_bridge_p4_case.scad` is a 3D-printable OpenSCAD case for the P4 board
(60 x 74.3 x 27.1 mm, snap-fit or M2.5 screws). Previews are `case/p4-preview-*.png` (P4) and
`case/s3-preview-*.png` (S3). The `.scad` file explains the print settings and the values marked
`MEASURE`. `case/wyze_esp_bridge_case.scad` is the S3 board case.

## A note on AI assistance

Parts of this project — code, configuration, and documentation — were written with the help of generative AI. Everything in here was reviewed, tested, and corrected by a human before being committed; nothing was accepted on the model's say-so.

## License

[BSD 2-Clause](LICENSE)