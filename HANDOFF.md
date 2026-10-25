# lorawan-for-esphome — session handoff

Self-contained brief for a fresh Claude Code session working in **this repo
only**. It assumes no access to the sibling WireStudio repo, so the
load-bearing context is inlined. Read this, confirm the spike goal still
holds, then start at *Next steps*.

## Mission

A LoRaWAN MAC component for ESPHome. It joins a LoRaWAN network over OTAA and
uplinks ESPHome sensor values, using [RadioLib](https://github.com/jgromes/RadioLib)
for the radio + MAC and ESPHome's own SPI, sensor, and preference machinery
for everything else.

ESPHome ships **raw LoRa radio** components (`sx126x`, `sx127x`) but **no
LoRaWAN MAC** — no OTAA join, frame counters, regional bands, or nonce
handling. RadioLib has the MAC; ESPHome has the radio driver + the entire
sensor ecosystem. This repo is the missing glue: a LoRaWAN component in
ESPHome's lifecycle. Confirmed (2026-06) that no such component exists yet, so
this is a real contribution, not a duplicate.

## Status

**Pre-alpha, tagged `v0.1.0`** (tag on ba7b756, GitHub release published).
CI compiles three configs against RadioLib
7.7.1 + ESPHome 2026.6.1 (SX1276 and SX1262 on Arduino, SX1262 on ESP-IDF) and
round-trips the payload codec (`.github/workflows/ci.yml`).

Hardware-validated on 2026-10-24: **v0.1.0 itself**, on the Heltec V4 /
SX1262 (US915 sub-band 2), Arduino and ESP-IDF. Session resume across a chip
reset and a true power pull, deep sleep, the radio task, the band-driven data
rate, typed payload fields and Class C (RxC delivery within a second) all
passed; see *HIL findings — v0.1.0*
below. On 2026-10-25 the **SX1276 path** passed too, on a TTGO LoRa32 T3
v1.6.1 (Arduino): fresh join, server-side decode, and session resume across a
chip reset and a true power pull. Still open on hardware: EU868 / AU915.

Two rules came out of those runs (both in the README): switching a board from
**ESP-IDF to Arduino** loses the stored nonces and session (different NVS
partition layout), and the first join after an esptool flash fails because the
board boots twice inside the join's RX windows (seen on the ESP32-S3 and on
the plain ESP32).

## Reuse-first, and the WireStudio boundary

The default is to reuse robust upstreams, not reimplement. Dependencies are
RadioLib + ESPHome, both well-established.

**This repo is the *device half* only.** It is consumed by a separate
orchestrator (WireStudio) that owns the *server half*:

- **Do build here:** the ESPHome component — radio init, OTAA join, FCnt,
  nonce/session persistence, packing sensor values into uplinks, the config
  schema, regional support.
- **Do NOT build here:** ChirpStack provisioning, device registration, key
  generation, DevNonce flushing, MQTT/HA confirmation. That is server-side and
  lives in the orchestrator. The component must never talk to a network
  server's management API. Keys arrive as config (`dev_eui` / `join_eui` /
  `app_key`, via `!secret`).

Keep that line clean: if a task tempts you toward "register the device" or
"talk to ChirpStack," it belongs in the orchestrator, not here.

## The spike goal (this gates everything)

One risk dominates and must be retired before any polish: **RX-window timing
under ESPHome's cooperative loop.** LoRaWAN class-A opens receive windows at
RX1 (+1 s) and RX2 (+2 s) after each uplink, and RadioLib's `sendReceive()`
blocks through them. ESPHome's `loop()` is cooperative — a long block stalls
WiFi, the API, and every other component. **Update:** the headless deployment
decision (no WiFi/API; see *Deployment model*) retired this for the shipped
profile, and #19 moved RadioLib onto its own task so `loop()` no longer blocks
at all (not yet run on hardware).

**Spike acceptance:**

1. ~~The component builds via `esphome compile`.~~ **Done** — CI-green.
2. On a TTGO LoRa32 v1 (SX1276), it OTAA-joins a real LoRaWAN network
   (US915, sub-band 2) and an uplink is received + decoded server-side.
   **Done on a Heltec V4 / SX1262** (2026-08-28 on RadioLib 7.2.1, 2026-10-24
   on `main` @ ca52396) and **on a TTGO T3 v1.6.1 / SX1276** (2026-10-25,
   v0.1.0).
3. WiFi/API stay responsive during the uplink burst. **Reframed:** the field
   profile is headless (no WiFi/API), and the radio task (#19) keeps `loop()`
   free regardless. *(see Deployment model)*
4. A **power cycle re-joins without a server-side nonce flush** — i.e. the
   DevNonce persistence works. *(Chip reset verified 2026-10-24: re-joined in
   8 s with no flush. A true power pull still needs a human at Wyola. Since
   #17 a reboot should resume the stored session with no join at all.)*

SX1262, a compact payload, and more regions are done in code; what gates the
spike is #2 on the TTGO and #4 with a real power pull.

## Key LoRaWAN findings — do not relitigate

Established through prior research; treat as given.

1. **DevNonce replay is the dominant OTAA failure mode**, not byte order.
   LoRaWAN requires a monotonically increasing DevNonce per JoinRequest.
   Servers reject a reused or lower nonce and **drop the join silently** — it
   looks identical to an auth failure. A naive firmware resets its counter to
   0 on every reboot/re-flash → rejected. **Fix (device side, this repo):**
   persist nonces in NVS so they survive reboots — done here via ESPHome's
   `ESPPreferences`. (The server-side companion fix — flushing nonces on
   reprovision — is the orchestrator's job, not this repo's.)
2. **A full-chip erase wipes NVS** and resets nonces. Re-flash the **app
   region only** to preserve NVS, or pair a full erase with a server-side
   nonce flush. Getting this wrong silently reintroduces failure #1.
3. **DevEUI from the ESP32 eFuse MAC.** The base MAC is factory-burned and
   readable even on a blank board. MAC-48 → EUI-64 by inserting `0xFF,0xFE`.
   Caveat: `getEfuseMac()` byte order changed across arduino-esp32 2.x — **one
   place is authoritative for the DevEUI; never derive it twice.** In this
   model the DevEUI arrives via config, so the component does not derive it.
4. **MSB key order.** RadioLib takes DevEUI / JoinEUI / AppKey in MSB order,
   matching the ChirpStack UI. Config hex is MSB.
5. **Region/sub-band must match the gateway.** US915 sub-band 2 is the only
   exercised band; EU868 and AU915 are accepted from RadioLib's band tables but
   have never transmitted. A wrong channel mask = the device transmits joins
   on channels the gateway never hears = a "won't join" that is not auth.

## What's in the scaffold

```
components/lorawan/
  __init__.py      # config schema (region, sub_band, EUIs/key, radio pins on an
                   # spi: bus of its own, deep_sleep_id) + to_code; pins
                   # jgromes/RadioLib 7.7.1
  esphome_hal.*    # RadioLib HAL over ESPHome SPI + GPIO (Arduino and ESP-IDF)
  sensor.py        # sensor sub-platform: bind an ESPHome sensor as a payload field
  payload.h        # field encodings (float32, scaled ints); host-testable
  lorawan.h        # LoRaWANComponent; radio request/result types
  lorawan.cpp      # setup() inits the radio and starts the radio task; loop()
                   # queues join/uplink requests and handles results (NVS,
                   # on_downlink); the task owns RadioLib; deep-sleep cycle
codec/
  decodeUplink.js  # reference ChirpStack codec (FIELDS in lockstep with the bindings)
example/
  spike-ttgo-lora32-v1.yaml     # bench config: WiFi + SX1276, keys via !secret
  field-headless.yaml           # production config: LoRaWAN-only, no WiFi/api/ota
  heltec-v4-node.yaml           # Heltec V4 / SX1262, the HIL-validated pin map
  multi-sensor.yaml             # several typed fields in one uplink
  lorawan-secrets.yaml.example  # per-device namespaced keys template
tests/
  ci-compile.yaml         # SX1276, Arduino, deep sleep
  ci-compile-sx1262.yaml  # SX1262 + TCXO/PA keys, Class C, send_raw, on_downlink
  ci-compile-idf.yaml     # the SX1262 config under ESP-IDF
  codec/                  # vectors.cpp packs with payload.h; check.js decodes
                          # with the codec
.github/workflows/
  ci.yml           # codec round-trip + esphome compile gate (pinned esphome)
README.md  LICENSE (MIT)  .gitignore  CLAUDE.md  HANDOFF.md (this file)
```

Payload encoding: each bound sensor's value is appended little-endian in
declaration order, as `float32` or a scaled, saturating integer with a
reserved "no reading" code (`components/lorawan/payload.h`; README, *Payload
encoding*). `tests/codec` packs vectors with that header on the host and
decodes them with `codec/decodeUplink.js` in CI. Generating the codec's
`FIELDS` from the YAML is the orchestrator's job.

## Known caveats

- ~~**RadioLib API is version-sensitive / unverified.**~~ **Resolved.** Verified
  against the pinned 7.7.1 headers and builds clean. Fixed: nonce-restore must
  run after `beginOTAA` (which clears nonces) and `setBufferNonces` validates the
  blob checksum against `keyCheckSum`; `activateOTAA` returns `NEW_SESSION` /
  `SESSION_RESTORED` (both negative), not `ERR_NONE`; `begin()` is on the concrete
  chip, not `PhysicalLayer`. Keep building to the pinned version.
- **RadioLib 7.2.1 -> 7.7.1.** 7.2.1 carries GHSA-xj4h-672x-ggwj: `parseDownlink`
  computes `payLen` as a `uint8_t` with no floor, so a short frame with a large
  FOptsLen underflows it. Reachable pre-MIC by anyone who knows the DevAddr (it
  is in clear on every uplink); before `verifyMIC` the underflowed value only
  drives the FPort read past the heap buffer, after which the frame fails MIC.
  Fixed in 7.7.0; 7.7.1 (#1792) frees the buffer on that new early return and
  bounds the reserved-FPort package index (direct indexing introduced after
  7.2.1, so not a 7.2.1 bug). Bump consequences:
  - The nonce buffer lost its 1-byte `ACTIVE` field (15 -> 14 bytes,
    `NONCES_VERSION_VAL` 2 -> 3). The NVS key is now `lorawan_nonces_v3`, so
    every already-flashed device starts with fresh DevNonces once and needs a
    one-time server-side nonce flush.
  - `RADIOLIB_LORAWAN_MAX_DOWNLINK_SIZE` (250) is gone; downlink buffers use
    `RADIOLIB_LORAWAN_MAX_PAYLOAD_SIZE` (242), what RadioLib itself sizes them
    to. `lenDown` is output-only in both versions (RadioLib zeroes it).
  - 7.7.0 deprecated the positional `begin(freq, bw, ...)` overloads (removed
    in 8.0.0). Migrated to `begin(const ConfigLoRa_t&)` with RadioLib's
    defaults, which match the old positional ones. Chip-specific knobs moved
    off `begin()` onto public members read during it: the SX1262 TCXO voltage
    is now `radio->tcxoVoltage`, set before `begin(cfg)`.
- **Radio task (not yet run on hardware).** RadioLib's join and uplink block
  through the RX windows (5-6 s on a join, 1-2 s on an uplink). That used to
  run in `loop()`/`setup()`: it starved every other component and tripped the
  Task WDT, first worked around by detaching the WDT (`WdtPause`), then by
  feeding it from RadioLib's sleep callback (HIL-validated 2026-10-24). Now a
  FreeRTOS task owns the radio: `loop()` queues join/uplink requests and
  handles the results (logs, NVS, `on_downlink`) on the main task. The task
  runs on the loop's core above it in priority; RadioLib sleeps through the
  long RX delays (when `loop()` runs) and only busy-waits, with `yield()`, for
  at most one frame's airtime. The task is not on the Task WDT and `loop()` no
  longer blocks, so the sleep callback is gone. `send_raw` is now queued: it
  returns once the request is accepted, not once it is sent. The radio's SPI
  bus must not be shared with anything else.
- **SPI ownership** — the radio is an ESPHome SPI device (`spi:` + `cs_pin`),
  reached through `EsphomeHal` (`esphome_hal.cpp`): ESPHome owns CS via
  `SPIDevice::enable()`/`disable()`, and RadioLib's pin numbers map to ESPHome
  pins. That is what makes the ESP-IDF build work. Because the radio task
  drives the bus concurrently with the main loop and ESPHome's SPI is not
  thread-safe, a final validate rejects any other SPI device on the radio's
  bus.
- **ESP-IDF -> Arduino loses NVS.** ESPHome's IDF partition table puts `nvs`
  at 0xf90000 (0x70000); the Arduino table has `eeprom`/`spiffs` there and
  `nvs` at 0xfa0000 (0x60000). Arduino -> IDF carries the session over (IDF's
  window contains Arduino's pages); IDF -> Arduino does not (HIL 4b). Treat it
  as a full erase: flush DevNonces server-side.
- **First join after an esptool flash fails.** The board boots twice a few
  seconds apart (ESP32-S3 over USB-Serial/JTAG, and the plain ESP32 behind a
  CP210x/CH9102 auto-reset circuit on the bench's serial proxy); the first
  boot sends a join-request and is reset inside the RX windows before the
  component has persisted the advanced DevNonce, so the second boot reuses it
  and the server rejects it. The next interval joins. A power cycle boots
  once. The nonce is saved when `activateOTAA()` returns; saving it right
  after TX would need a hook RadioLib does not expose.
- **SX1276 has run on Arduino only.** The SX127x path (`dio0_pin`, no BUSY)
  passed on the TTGO T3 v1.6.1 under Arduino; under ESP-IDF it is
  compile-checked only.

## Next steps (ordered)

1. **EU868 / AU915 on a real gateway**; drop their untested warning once each
   has joined.
2. **Upstreaming** to ESPHome as an official component.

Loose ends from the session that produced v0.1.0:

- A reply in the ESPHome Discord `#external-components` thread ("Why does it
  need to block?") describes v0.1.0 and lists the bench run below as in
  progress; it promises a follow-up with the results.
- Workflow used so far: one PR per change from the working branch, squash-merged
  once CI is green. After a squash merge, rebase follow-on local work with
  `git rebase --onto origin/main <old-base>`, not a plain rebase, or the
  squashed commits replay and conflict.
- The cloud sessions could not run a full `esphome compile` (the PlatformIO
  registry was blocked; CI was the compile gate) and could not push tags. A
  session on the workbench VM has neither limit.

## HIL findings — v0.1.0 on the TTGO T3 v1.6.1 / SX1276 (2026-10-25)

Board: WireStudio's spare TTGO LoRa32 T3 v1.6.1 (ESP32-PICO-D4, SX1276,
CH9102 console) on Wyola SLOT14's neighbour SLOT19, DevEUI `64b708fffeab8974`,
US915 sub-band 2, Arduino. Config: roboat's `spare-ttgo-t3-battery.yaml`
(`spare-t3-node`, no sensor fields, an 8-byte fPort 1 `send_raw` every 5 min)
moved to v0.1.0: top-level `spi:`, the component at the v0.1.0 tree. The
board had run this component at b7da617 (pre radio task) since 2026-09-27, so
the SX127x path was not new to the air; v0.1.0 was. Gateway RSSI -58, SNR
10-13 at bench range.

1. **Fresh join -- pass.** Nonces flushed 13:54, flash with erase 14:10.
   First boot's join-request was accepted server-side 14:10:25 (JOIN, addr
   00f90885) but the board rebooted 3 s later, re-sent DevNonce 0 and got
   "DevNonce has already been used"; `OTAA join failed: -1116` at 14:10:34.
   The retry one interval later joined: JOIN 14:15:28 (addr 00d7f692),
   `OTAA join OK (new session)` 14:15:33.
2. **Uplink decoded server-side -- pass.** fCnt 0 at 14:15:47, DR0 (8 bytes
   stays under the US915 DR0 limit), decoded by the profile codec to
   `{uptime_s: 319, boot_count: 2, batt_mv: 4252}`; `boot_count: 2` is the
   double boot of test 1.
3. **Chip reset -- pass.** `serial_reset` at 14:17:45: `OTAA join OK
   (restored)`, fCnt 1 at 14:17:49 on the same address, no JOIN event. The
   bench reset also produced two boots (fCnt 2 at 14:17:58, `boot_count` 4),
   both restored.
4. **True power pull -- pass.** USB power to the board's hub port off 14:19:52
   (port status `off`, console gone), on 14:19:59: one boot, session
   restored, fCnt 4 at 14:20:19, no JOIN event.

Left out: deep sleep, Class C, typed fields and the data-rate ladder were
covered on the Heltec V4 and are chip-independent above the radio layer; the
T3's regulator makes deep sleep moot anyway (see *Board power floor*).
Handed back 14:20 on this build with the same identity.

Also observed: the first HIL flash used the wrong secrets (`t3b_*` from the
ESPHome dashboard, which are DevEUI `500291fffe9df404`) and joined as that
device for ten minutes; that record now holds a stale session, harmless while
that board is off the air. The `t3_*` keys for this board live in the roboat
repo's git-ignored `node-firmware/secrets.yaml`.

## HIL findings — v0.1.0 on the Heltec V4 (2026-10-24)

The v0.1.0 plan (eight tests) was run in full; all eight passed.

Tests 1-4 run by the "Homelab working session", 5-8 by this session, both on
the Wyola workbench SLOT14, all times UTC. Build: ESPHome 2026.8.1 (CI pins
2026.6.1), `external_components` pinned `@v0.1.0` (ba7b756), RadioLib 7.7.1.
Config from `example/heltec-v4-node.yaml`: top-level `spi:` (clk 9, miso 11,
mosi 10), `setup_high: [GPIO7, GPIO2]`, `setup_low: [GPIO36]`, `txen_pin:
GPIO46`, `dio2_as_rf_switch`, `tcxo_voltage: 1.8`, Class A, `uplink_interval:
1min`, one float32 battery field unless stated. Flashed as a factory image at
0x0 without erasing NVS except where stated; nonces flushed once before the
first flash. Gateway-side evidence from ChirpStack's device event stream
(MQTT `application/+/device/+/event/+`).

1. **Regression (Arduino) -- pass.** Post-flash boot joined at 15:53:34
   (DevAddr 00678317); uplinks 15:54:41, 15:55:41, 15:56:41 fCnt 0-2, DR0,
   4 B. fport 10 `[0a 0b 0c]` queued 15:54: `downlink fport=10 (3 bytes)` and
   `on_downlink fired`. `Setup lorawan took 62ms`.
2. **Loop no longer blocks (#19) -- pass.** As `8cfd49fffeb5dead` (reset
   16:05:24, `stored session not usable (-1120), joining fresh`): `OTAA join
   failed: -1116` at 16:05:44, 16:06:44, 16:07:44, 16:08:44, 16:09:43. No
   `took a long time for an operation` anywhere in the run on either
   framework (ca52396 logged 6.6 s per failed join and 1.4 s per uplink), no
   `task_wdt`, no panic, no reset.
3. **Session persistence (#17).**
   a. Chip reset (`rst:0x15`) -- pass on Arduino (15:57:50: `OTAA join OK
      (restored)`, fCnt 4->5 on 00678317, no join-request) and on ESP-IDF
      (16:20:51: restored, fCnt 8->9 on 0125cae4).
   b. True power pull -- pass. Uplink fCnt 13 at 16:27:04; VBUS cut
      16:27:11-16:27:18 (`uhubctl -l 1-1.2 -p 1 -a off/on` on wyolora; both
      devices on that hub vanished, battery ADC read ~0.04 V); next uplink
      fCnt 14 at 16:28:22 on 0125cae4, no join.
   c. Power pull after a fresh join, before any uplink -- pass. Nonces flushed,
      flashed with erase; JOIN 16:30:59 (0193901d), VBUS cut 16:31:13-16:31:20;
      first uplink fCnt 0 at 16:32:24 on 0193901d, no join-request.
4. **ESP-IDF (#20) -- pass, best case.** IDF image flashed over the Arduino
   build at 16:17:25 without erasing. Arduino's last uplink fCnt 4 at 16:17:19,
   IDF's first fCnt 5 at 16:18:45, same DevAddr 0125cae4, no join: IDF reads
   the Arduino build's NVS. No flush rule needed in this direction; the
   reverse is 4b.
   Test 1 repeated on IDF: downlink at 16:22:05 + `on_downlink`; `Setup
   lorawan took 67ms`.
4b. **ESP-IDF -> Arduino (reverse of test 4) -- session and nonces lost.**
   Registered IDF build (session 00ab1549, fCnt 1 at 17:20:42); Arduino build
   flashed over it at 17:21:10 without erasing, same keys. It came up with
   DevNonce 0: nonces 0, 0 (double reset), 1 rejected as "already used" at
   17:21:05, 17:21:07, 17:22:07; no `stored session not usable` line, so the
   nonce blob was not found at all. Cause: ESPHome's partition tables differ.
   ESP-IDF: `nvs` at 0xf90000, 0x70000 long. Arduino: `eeprom` 0xf90000,
   `spiffs` 0xf91000, `nvs` at 0xfa0000, 0x60000 long. NVS allocates pages
   from the start of its partition, so IDF's data lives in the 64 KiB that
   Arduino does not scan; Arduino's data lives inside IDF's window, which is
   why Arduino -> IDF works. Rule: switching ESP-IDF -> Arduino is a
   full-erase equivalent; flush DevNonces server-side (or wait for the
   restarted counter to pass the old nonces, one interval each).
5. **Deep sleep (#18, ESP-IDF, typed 5-byte payload) -- pass.** `sleep_duration:
   2min`, flashed 16:38:00 over the IDF build without erasing. Wakes at
   16:38:11, 16:40:19, 16:42:23: each `uplink sent (5 bytes)` then `Beginning
   sleep` / `Sleeping for 120000000us`; fCnt 6, 7, 8 on 0193901d, no
   join-request on any wake. Downlink queued 16:38:57 (between wakes) arrived
   in the 16:40:19 wake: `downlink fport=10 (3 bytes)`, `on_downlink fired`,
   TXACK at the gateway.
   Failed-join case (`8cfd49fffeb5dead`, same deep-sleep config, flashed
   16:50:10): wakes 16:50:15 and 16:52:2x each logged `OTAA join failed:
   -1116`, `not joined; sleeping, the next wake retries the join`, `Beginning
   sleep`: one join attempt per wake (~129 s apart), never stuck awake.
   Also observed: an ESP32-S3 on USB-Serial/JTAG drops off the USB bus while
   asleep, so the slot reads `absent` for ~115 s of each cycle, the first
   ~1-2 s of every wake's log are lost before the slot proxy reattaches
   (`rst:` line and `OTAA join OK (restored)` not captured; the gateway's
   fCnt continuity is the evidence), and flashing out of a deep-sleep build
   must hit the ~4-8 s wake window.

6. **Data rate from the band (#21, Arduino) -- pass.** Three `float32` fields
   (12 B) plus a 60-byte `lorawan.send_raw` on fport 10 every 150 s. Joined
   17:23:07 (00623c63). 12 B uplinks at **DR1** (fCnt 0, 1, 3 at 17:24:07,
   17:25:07, 17:26:09), the 60 B uplink at **DR2** (fCnt 2, 17:25:09, bytes
   `00..3b`), and every 4 B / 5 B uplink in the whole run at **DR0** with
   `adr: true` throughout. No `uplink failed: -4`; no untested-region warning
   on US915.
7. **Compact payload (#22, ESP-IDF, from the deep-sleep build) -- pass.** Boot
   log `payload: 3 fields, 5 bytes`; `frm_payload` `2c005908ff` /
   `35005908ff` at DR0 = battery 0x002c / 0x0035 mV (uint16 x1000; the ADC
   pin reads ~0.04 V on this board), `5908` = 2137 (int16 x100 of 21.37), `ff`
   = the uint8 "no reading" code for a NaN template sensor. Decoded locally
   with `codec/decodeUplink.js` and the matching `FIELDS`:
   `{battery: 0.044, t: 21.37, n: null}`.
8. **Class C on the radio task (Arduino) -- pass.** Class C build flashed
   17:26:45 over the Arduino test 6 build: `OTAA join OK (restored)` then
   `device class C enabled` (seen on a harness reset at 17:30; the post-flash
   boot's lines were lost as usual), uplinks continuing on 00623c63. With the
   bench device profile still `supportsClassC: false`, a downlink queued 11 s
   after an uplink rode the next uplink (Class A path, 17:28:31). ChirpStack
   assigns the device class **at join time** from the profile, so enabling
   Class C on the profile did not change a restored session (`classEnabled`
   stayed `CLASS_A` through two more queued downlinks, both delivered on the
   following uplink). After a nonce flush and a flash with erase: JOIN
   18:20:03 (01d339ff), `classEnabled: CLASS_C`, uplink fCnt 0 at 18:21:03,
   downlink queued 18:21:14.35, TXACK 18:21:14, device `class C downlink
   fport=10 (3 bytes)` + `on_downlink fired` at 18:21:14.88. The RxC poll on
   the radio task delivers within a second, 11 s outside any Class A window.

Also observed, explained: the first join after an esptool flash fails, and the
server sees the same DevNonce twice ~2-3 s apart (16:29:57/16:29:59 and
17:17:40/17:17:43; the gateway is a single Wyolora gateway). esptool's
post-write "hard reset" over USB-Serial/JTAG resets the ESP32-S3 twice: the
app boots, sends a join-request (DevNonce n), and is reset again
(`rst:0x3 (RTC_SW_SYS_RST)`) inside the join's RX windows. RadioLib advances
the DevNonce in RAM right after TX, but the component persists it only when
`activateOTAA()` returns, so the second boot reuses n, ChirpStack logs
"DevNonce has already been used", the device logs `-1116`, and the next
interval joins with n+1. Harmless in the field (a reset inside a 6 s window),
and the same mechanism behind the 2026-08-28 "first join after power-up fails"
quirk. The one real consequence: a join-request that the server accepted
(DevAddr allocated) is abandoned, which is just an orphan session server-side.

Also observed: the ChirpStack codec on that profile expects 12 bytes (the GNSS
gate layout), so every 4 B and 5 B test uplink logged an `UPLINK_CODEC` error
server-side. Expected; not the component.

Board at handover (18:25): SLOT14 on the v0.1.0 Arduino Class C build
(`device_class: C`, 1 min, one float32 field), DevEUI `8cfd49fffeb55758`,
fresh session 01d339ff after a full erase, nonces flushed 18:18, and the
device profile temporarily set to Class C for test 8. Handed to the homelab
session, which replaced the 137d06e gate firmware with a v0.1.0 gate config
instead of restoring it, reset the profile to Class A, and confirmed the
reverse of the test 8 rule: the Class A build over the Class C one restored
session 01d339ff and ChirpStack kept `classEnabled: CLASS_C`; only an erase
plus nonce flush produced a fresh Class A join (01cf0b19, 21:04).

## Deployment model (decided)

Field firmware is **headless: no `wifi:`, no `api:`, no network `ota:`** — serial
`logger:` only. Rationale: LoRaWAN nodes are usually out of WiFi range and on
battery/solar, the keys are compile-time (`!secret`), and ESPHome's `wifi:`/`api:`
both default to `reboot_timeout: 15min` — a configured-but-unreachable WiFi
reboot-loops the device (and every reboot re-joins, burning a DevNonce). So
build → flash → join → update are all **wired (USB serial)**; the join is observed
on the serial log. WiFi only earns its keep on the bench.

### Open considerations

- **Deep sleep (implemented, #18; not yet run on hardware).** The real power
  lever. Built as `deep_sleep_id` pointing at ESPHome's own `deep_sleep`, so wake
  sources, shutdown hooks and `deep_sleep.prevent` come from upstream; see the
  README for the rules. Each wake: restore nonces + session -> `activateOTAA()`
  (`SESSION_RESTORED`, no join airtime, in steady state) -> wait up to 10 s for
  the bound sensors -> one uplink -> save + `global_preferences->sync()` ->
  `radio_->sleep()` (an SX127x left in standby idles at ~1.5 mA) ->
  `begin_sleep()`. FCntUp must stay monotonic across wakes or the server drops
  frames as replays, which is why every save is synced at once.

- **Board power floor (TTGO LoRa32 v1.6.1).** Even with deep sleep this board
  does not reach µA: the AMS1117 LDO quiescent (~5-10 mA) and the power LED
  (~1-3 mA) draw continuously regardless of CPU/radio state, so stock deep-sleep
  current is single-digit mA. True low power needs a low-quiescent board or
  hardware mods (bypass the LDO, lift the LED). Without deep sleep, idle between
  uplinks is dominated by the always-active CPU at ~40-60 mA — order ~2 days on
  an 18650, which is why deep sleep is the preferred direction.

- **WiFi-on-demand via downlink (future).** Keep field firmware LoRaWAN-only, but
  let a Class-A *downlink* command flip on WiFi for maintenance: device receives a
  downlink → enables WiFi (`wifi.enable`) → performs OTA → uplinks an
  update-confirmation → receives a downlink to disable WiFi (`wifi.disable`) →
  returns to LoRaWAN-only. Gives remote OTA without a permanent WiFi power/airtime
  cost. Needs: downlink parsing/command dispatch, the WiFi enable/disable plumbing
  (with `reboot_timeout: 0s` so the maintenance window doesn't reboot-loop), and a
  server side that schedules the downlink + watches for the confirmation uplink
  (orchestrator's half).

- **Per-device secrets scaling.** Keys live in a gitignored `lorawan-secrets.yaml`
  merge-included from `secrets.yaml`, namespaced per device (`<node>_dev_eui`,
  etc.; see README). This is fine for a handful of devices but gets unwieldy as
  the fleet grows — the `<node>_` prefix is repeated by hand (substitutions can't
  be used inside `!secret`), and one flat file holds every device's keys. If the
  device count climbs, revisit: a generator that emits per-device configs +
  secrets from a manifest (orchestrator's job), or a packages-based layout.

## Build / test

```bash
# from the repo root, with a secrets.yaml beside the example
esphome compile example/spike-ttgo-lora32-v1.yaml
```

```bash
# what CI runs
g++ -std=c++17 -Icomponents/lorawan -o /tmp/vectors tests/codec/vectors.cpp
/tmp/vectors | node tests/codec/check.js
esphome compile tests/ci-compile.yaml   # also -sx1262, -idf
```

Conventions live in [`CLAUDE.md`](CLAUDE.md). Pin a tag (`v0.1.0`) or a commit
when consuming this repo elsewhere — never `main`.

## HIL findings — Heltec V4 on the Wyola workbench (2026-08-27)

First hardware session against the real gateway (SLOT28 on wyola-workbench,
ChirpStack device `8cfd49fffeb55758` "Heltec WiFi LoRa 32 V4 GNSS"). Facts,
each verified at the gateway side by decoding `us915_1/gateway/+/event/up`
protobufs and reading ChirpStack's device DB:

- **OTAA join-requests transmit and are ACCEPTED server-side** — 69+ burned
  dev-nonces, dev_addr allocated, join-accepts transmitted (RX1 926.9 MHz
  SF9/500k). The MIC/keys/registration are all correct.
- **The device hears none of them.** With the original 4-pin `setup_high`
  the join loop hung silently; with corrected FEM pins it fails fast and
  clean with `RADIOLIB_ERR_NO_JOIN_ACCEPT (-1116)` every cycle.
- **TX RSSI at the gateway (~2 m away) is -52 dBm** in every working pin
  configuration — leakage-grade for a 28 dBm PA board. Driving VEXT low with
  the TX-path pins floating dropped it to **-122** (powered FEM blocking an
  unselected path), which proves the pins do reach the FEM.
- **Prime suspect: antenna missing/unseated** (u.FL) — weak TX + total RX
  deafness + insensitivity to front-end permutations all fit. Physical check
  pending. The V4 also has a separate GNSS u.FL; easy to confuse.
- Pin map for both V4 FEM revisions documented in
  `example/heltec-v4-node.yaml`; `setup_low:` was added for VEXT-class
  active-low rails.
- Closed-loop iteration works end to end: `esphome compile` on the dev box,
  flash over the workbench API (esptool to SLOT28), serial monitor with
  pattern match, gateway-side RF decode. One cycle ≈ 4 minutes.

## 2026-08-28: the V4 deafness fix (mechanism later disproved)

The user called it: the board RF'd fine under previous firmware, so hardware
was never the suspect. Root cause -- **GPIO46 (GC1109 `PA_TX_EN`) is a
per-transfer pin and every earlier config drove it statically**. Held high, the
PA output stage stays engaged and the receiver is deaf: join-requests transmit
(69 dev-nonces consumed server-side), join-accepts never arrive (-1116).
Meshtastic's variant.h names the pins (`LORA_GC1109_PA_EN 2`,
`LORA_GC1109_PA_TX_EN 46`) and MeshCore PR #1249 describes the same fix:
give the pin to RadioLib's rf-switch handling so it toggles with TX/RX.

The component now has `rxen_pin`/`txen_pin` (-> `Module::setRfSwitchPins`).
Proven config: `setup_high: [GPIO7, GPIO2]`, `setup_low: [GPIO36]`,
`txen_pin: GPIO46`, `dio2_as_rf_switch: true`. Bench result, closed loop:
OTAA join OK (dev_addr allocated, f_cnt advancing), Class C enabled, and a
queued downlink (fport 10) received and dispatched to `on_downlink`.

Two quirks worth knowing:
- The first join attempt after power-up consistently fails -1116; the retry
  one uplink_interval later succeeds. Unexplained -- possibly FEM settle
  after VEXT power-on. Harmless with the existing retry loop. **Did not
  reproduce on 2026-10-24** (RadioLib 7.7.1): 3 of 3 first joins succeeded.
- A Class A device profile server-side means downlinks ride the next uplink's
  RX window even when the device runs Class C locally; immediate delivery
  needs `supports_class_c` in the ChirpStack device profile too.

**Revisited 2026-10-24: the mechanism above is wrong.** The GC1109 truth table
(quoted in Meshtastic `variants/esp32s3/heltec_v4/variant.h`) has CPS (GPIO46)
as don't-care in receive -- it only selects full PA vs bypass during transmit,
and TX/RX path select is CTX, driven by SX1262 DIO2. A static-high GPIO46 cannot
deafen the receiver, and the gate board running 137d06e with GPIO46 (and GPIO5)
held statically high joined at 13:51 UTC. `txen_pin: GPIO46` is correct **on a
V4.2** (full PA in TX, nothing in RX), but it is not what fixed the August
deafness, and the bench board turned out to be a V4.3, where the PA pin is
GPIO5 (see "The bench V4 is a V4.3").
b7da617 changed several things at once; the real cause is unrecorded. See the
2026-10-24 HIL findings.

## HIL findings — main @ ca52396 on the Heltec V4 (2026-10-24)

Run by the "Homelab working session" on the Wyola workbench, SLOT14, from the
brief in this session; serial evidence cross-read from the slot's buffer.
Build: ESPHome 2026.8.1 (CI pins 2026.6.1), RadioLib 7.7.1 fetched by
PlatformIO. Config: the GNSS-16M board's pins with the proven radio block
(`setup_high: [GPIO7, GPIO2]`, `setup_low: [GPIO36]`, `txen_pin: GPIO46`,
`dio2_as_rf_switch`, `tcxo_voltage: 1.8`), `uplink_interval: 1min`, Class A.
Flashed as a full factory image at 0x0 **without** erasing NVS, so the 7.2.1
nonce blob was still on flash and the new `lorawan_nonces_v3` key started
fresh; DevNonces were flushed server-side once before the first flash. Every
boot's reset reason was `rst:0x15 (USB_UART_CHIP_RESET)`, i.e. the harness.

1. **Radio init -- pass.** `sx1262 begin with tcxo 1.80V` on every boot, no
   `radio begin failed`. `ConfigLoRa_t` + `radio->tcxoVoltage` work.
2. **Join + uplink -- pass.** `OTAA join OK (new session)` at 13:30:17 on the
   first attempt; `uplink sent` every 60 s as `8cfd49fffeb55758`; ChirpStack
   `f_cnt_up` 3 at 13:33.
3. **Downlink -- pass.** fport 10 `[0a 0b 0c]` enqueued over gRPC; 13:31:13
   `downlink fport=10 (3 bytes)` then `on_downlink fired: port=10 len=3`;
   queue drained.
4. **Task WDT -- pass.** Re-flashed as the unregistered DevEUI
   `8cfd49fffeb5dead` (ChirpStack NOT_FOUND): six consecutive `OTAA join
   failed: -1116` from 13:41:04 to 13:46:04, each blocking 6623 ms (ESPHome's
   `took a long time for an operation` warning), with no `task_wdt`, no panic,
   and no reset line between them. Before #13 this case reboot-looped.
5. **Nonce persistence -- pass for a chip reset.** Harness reset at 13:34:10
   with no server flush; `OTAA join OK` at 13:34:18 with a new dev_addr. A
   true power pull is still unverified; NVS should not care.

Also observed:
- `loop()` still blocks 6.6 s on a failed join and 1.4 s on an uplink with RX
  windows. The sleep callback keeps the WDT fed; other components starve for
  that long. (Since moved to a radio task, #19.)
- `tests/secrets.yaml` carries DevEUI `70b3d57ed0000000`, which is registered
  nowhere; a build from it cannot join. The registered V4 identity is
  `8cfd49fffeb55758` (ChirpStack app `wirestudio`).
- ChirpStack REST is on port 8090 of the ChirpStack host (`:8080` is gRPC/UI). The nonce
  flush is `DeviceService.FlushDevNonces`; wirestudio wraps it as
  `ChirpStackClient.flush_dev_nonces()`. Server side, not this repo.
- The GNSS-16M gate config pins 137d06e (before `setup_low`/`txen_pin`
  existed) and holds GPIO7, GPIO2, GPIO5 and GPIO46 statically high with no
  VEXT drive. It joined at 13:51 after being restored. On a V4.2 GPIO5 is
  unconnected; on a V4.3 GPIO5 is KCT8103L CTX, and high means RX bypass
  (21 dB less LNA gain), so that config would receive weaker than it could.
  The board is a V4.3 by measurement; see the next section.

Final state: SLOT14 restored to its gate firmware (137d06e, RadioLib 7.2.1,
5 min uplinks), nonces flushed before the restore, joined 13:51. `main` is not
deployed on any gate board.

## The bench V4 is a V4.3: the PA pin is GPIO5 (2026-10-24)

Identified by RF behaviour, not by the silkscreen (nobody has read it yet; a
silkscreen read would only confirm). Same board (SLOT14), same gateway about
2 m away, `main` @ ca52396, US915 sub-band 2, all builds with
`setup_high: [GPIO7, GPIO2]`, `setup_low: [GPIO36]`, `dio2_as_rf_switch`,
`tcxo_voltage: 1.8`. Gateway-side RSSI/SNR per uplink from ChirpStack's frame
stream:

| Build (UTC window) | PA-related pins | Joins / RX | Gateway RSSI | SNR |
|---|---|---|---|---|
| A, 13:35-13:39 | `txen_pin: GPIO46` | OK | -110 to -114 dBm | ~3 |
| B, 14:31-14:35 | `txen_pin: GPIO5` | OK | **-58 to -67 dBm** | ~13 |
| C, 14:36-14:41 | GPIO46 held high (`setup_high`), no `txen_pin` | OK, not deaf | -110 to -118 dBm | -3 to 3 |
| gate 137d06e, 13:56-14:21 | GPIO5 + GPIO46 held high | OK | -58 to -72 dBm | ~12 |

GPIO5 raised for TX is worth about 45-50 dB at the gateway; GPIO46 makes no
difference either way. That is the V4.3 (KCT8103L) signature: GPIO5 is CTX,
GPIO46 is not connected to the FEM. On a V4.2 the result would invert.

Consequences:
- The example now takes the PA pin from a `pa_pin` substitution (GPIO46 for a
  V4.2, GPIO5 for a V4.3) and the README says so. A wrong pin is invisible on
  the bench -- the link still closes at 2 m -- and costs the range.
- Holding GPIO46 high does not deafen a V4.3 (build C joined first try and
  received normally), which is the second half of why the August mechanism
  could not have been right on this board.
- Holding GPIO5 statically high (the 137d06e gate config) works at 2 m but
  keeps the TX path selected in RX; per-transfer `txen_pin: GPIO5` is the
  right shape. The gate config moves to v0.1.0 with `txen_pin: GPIO5`.
- The v0.1.0 HIL runs on SLOT14 used `txen_pin: GPIO46` as briefed, so their
  uplinks went out PA-bypassed (about -105 to -109 dBm, DR0 through 16:35 UTC). That does
  not affect the pass/fail results, but it means ADR never had headroom to
  raise the data rate in those runs.

## Sources

- ESPHome LoRaWAN tracking: https://github.com/esphome/feature-requests/issues/2634
- RadioLib (LoRaWAN MAC): https://github.com/jgromes/RadioLib
- RadioLib DevNonce-on-power-cycle (TTGO LoRa32): https://github.com/jgromes/RadioLib/issues/1480
- ESPHome external components: https://esphome.io/components/external_components/
- ESP32 eFuse base MAC: https://github.com/espressif/esp-idf/blob/master/components/esp_hw_support/include/esp_mac.h
- arduino-esp32 getEfuseMac byte-order issue: https://github.com/espressif/arduino-esp32/issues/6458
