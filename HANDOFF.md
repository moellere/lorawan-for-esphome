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

Last hardware-validated build: `main` @ ca52396 on the Heltec V4 / SX1262,
2026-10-24. It OTAA-joined, uplinked, received a downlink, re-joined after a
chip reset with no nonce flush, and survived six failed joins without a
watchdog reset (*HIL findings (2026-10-24)* below).

Merged since, and **not yet run on hardware**:

| PR | Change |
|---|---|
| #17 | Session (DevAddr, keys, FCnt) persisted in NVS and synced after every join and uplink: a reboot resumes with no join |
| #18 | Deep sleep via ESPHome's `deep_sleep` (`deep_sleep_id`): one uplink per wake |
| #19 | RadioLib on its own FreeRTOS task; `loop()` no longer blocks |
| #20 | RadioLib HAL over ESPHome SPI/GPIO; ESP-IDF builds; SPI pins move to `spi:` (breaking) |
| #21 | EU868 and AU915 (untested, warned); uplink data rate taken from the band |
| #22 | Typed, scaled payload fields (`type`, `multiplier`) |

#23 is docs plus `send_raw` registered `synchronous=True`; no behaviour
change. The plan to validate all of it is *HIL plan — v0.1.0* below. Also
still open on hardware: a true power pull (only a chip reset has been tested)
and the TTGO LoRa32 / SX1276 path.

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
   on `main` @ ca52396); TTGO / SX1276 still pending.
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
- **SX1276 is unexercised on hardware.** Every HIL run so far is the Heltec V4
  / SX1262. The SX127x path (`dio0_pin`, no BUSY) compiles in CI but has never
  joined.

## Next steps (ordered)

1. **Run the *HIL plan — v0.1.0*** (below) on the Heltec V4. Write the results
   up as *HIL findings — v0.1.0* in the shape of the ca52396 section, fix what
   fails, and decide from test 4 whether the README needs a "framework switch
   means a nonce flush" rule.
2. **TTGO LoRa32 v1 / SX1276** — first join on the SX127x path, US915 sub-band 2.
   Register the device (MAC version 1.0.4) and flush its nonces once first.
3. **EU868 / AU915 on a real gateway** of that plan; drop their untested
   warning once each has joined.
4. **Upstreaming** to ESPHome as an official component, once 1-2 pass.

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

## HIL plan — v0.1.0

Heltec V4 in SLOT14 on the Wyola workbench, same harness as the 2026-10-24 run.
Nothing below has run on hardware.

**Build.** `external_components: source: github://moellere/lorawan-for-esphome@v0.1.0`
(or the local checkout). Start from `example/heltec-v4-node.yaml`: SPI pins in
the top-level `spi:` block (`clk_pin: GPIO9`, `miso_pin: GPIO11`, `mosi_pin:
GPIO10`), `cs_pin` under `radio:`, and the proven radio block unchanged
(`setup_high: [GPIO7, GPIO2]`, `setup_low: [GPIO36]`, `txen_pin: GPIO46`,
`dio2_as_rf_switch: true`, `tcxo_voltage: 1.8`). Registered DevEUI
`8cfd49fffeb55758`; failed-join case `8cfd49fffeb5dead`. Flash **without**
erasing NVS. SLOT14 runs 137d06e (RadioLib 7.2.1), whose nonce blob 7.7.1
cannot read, so flush DevNonces server-side once before the first flash.
`uplink sent` / `downlink` log at DEBUG. Arduino, `uplink_interval: 1min`,
Class A unless a test says otherwise.

1. **Regression.** `sx1262 begin`, `OTAA join OK (new session)`, `uplink sent
   (N bytes, fport=1) dev_eui=8cfd49fffeb55758` every 60 s; a queued fport 10
   `[0a 0b 0c]` arrives as `downlink fport=10 (3 bytes)`.
2. **Loop no longer blocks (#19).** No `took a long time for an operation`
   (ca52396 logged 6623 ms per failed join, ~1.4 s per uplink). As
   `…dead`: `OTAA join failed: -1116` each interval, no `task_wdt`, panic or
   reset over at least 3 failures.
3. **Session persistence (#17).** Record each `rst:0x..`.
   a. Chip reset after a join and 2+ uplinks: `OTAA join OK (restored)`, no
      join-request at the gateway, same DevAddr, `f_cnt_up` continues.
   b. True power pull within ~10 s of an `uplink sent`: `restored`, same
      DevAddr, FCnt not backwards.
   c. Power pull after a fresh join, before the first uplink: `restored`.
4. **ESP-IDF (#20).** Same YAML with `framework: type: esp-idf`, flashed over
   the Arduino build without erasing. Record the first boot: `restored` means
   IDF read the Arduino NVS; otherwise the join is silently rejected (`-1116`,
   DevNonce reuse), so flush once and let it join. Then repeat 1 and 3a.
5. **Deep sleep (#18).** `deep_sleep: {id: deep_sleep_1, sleep_duration: 2min}`
   and `lorawan: deep_sleep_id: deep_sleep_1`. Per wake: `restored`, `uplink
   sent`, sleep; one uplink per 2 min, FCnt +1 per wake, no join after the
   first. A downlink queued between wakes lands in the next wake. As `…dead`:
   `OTAA join failed` then `not joined; sleeping, the next wake retries the
   join`, never stuck awake. Native-USB serial re-enumerates each wake.
6. **Data rate (#21).** US915 payload limits DR0 11 B, DR1 53 B, DR2 125 B,
   DR3 242 B. 4 bytes: DR0 every uplink, even after ADR, no `uplink failed:
   -4`. 12 bytes (three `float32`): DR1. 60-byte `send_raw`: DR2 (the old
   table picked DR3). Optional: an 11-byte payload goes DR1 on the uplink
   after a LinkADRReq (FOpts). No untested-region warning on US915.
7. **Compact payload (#22).** Bind battery `uint16` ×1000, a template
   returning 21.37 as `int16` ×100, and a template returning `NAN` as
   `uint8`. Boot log `payload: 3 fields, 5 bytes`; `frm_payload` = battery mV
   LE, `59 08`, `ff`; DR0. Decode locally with `codec/decodeUplink.js`
   (`tests/codec/check.js` shows how), not by editing the shared ChirpStack
   device profile: expect `{battery: ~3.x, t: 21.37, n: null}`.
8. **Class C on the radio task (optional).** `device_class: C`, no deep sleep:
   after the first post-join uplink, a queued downlink arrives as `class C
   downlink fport=10 (3 bytes)` within seconds.

Afterwards restore SLOT14 to its gate firmware (137d06e), flushing nonces
before the restore.

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
held statically high joined at 13:51 UTC. `txen_pin: GPIO46` is still correct
(full PA in TX, nothing in RX) but it is not what fixed the August deafness.
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
- ChirpStack REST is on `10.254.0.11:8090` (`:8080` is gRPC/UI). The nonce
  flush is `DeviceService.FlushDevNonces`; wirestudio wraps it as
  `ChirpStackClient.flush_dev_nonces()`. Server side, not this repo.
- The GNSS-16M gate config pins 137d06e (before `setup_low`/`txen_pin`
  existed) and holds GPIO7, GPIO2, GPIO5 and GPIO46 statically high with no
  VEXT drive. It joined at 13:51 after being restored. On a V4.2 GPIO5 is
  unconnected; on a V4.3 GPIO5 is KCT8103L CTX, and high means RX bypass
  (21 dB less LNA gain), so that config would receive weaker than it could.
  Which revision the board is has not been read off the silkscreen.

Final state: SLOT14 restored to its gate firmware (137d06e, RadioLib 7.2.1,
5 min uplinks), nonces flushed before the restore, joined 13:51. `main` is not
deployed on any gate board.

## Sources

- ESPHome LoRaWAN tracking: https://github.com/esphome/feature-requests/issues/2634
- RadioLib (LoRaWAN MAC): https://github.com/jgromes/RadioLib
- RadioLib DevNonce-on-power-cycle (TTGO LoRa32): https://github.com/jgromes/RadioLib/issues/1480
- ESPHome external components: https://esphome.io/components/external_components/
- ESP32 eFuse base MAC: https://github.com/espressif/esp-idf/blob/master/components/esp_hw_support/include/esp_mac.h
- arduino-esp32 getEfuseMac byte-order issue: https://github.com/espressif/arduino-esp32/issues/6458
