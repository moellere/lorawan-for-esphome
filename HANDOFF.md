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

**Pre-alpha; compiles, not yet hardware-validated.** The component builds clean
via `esphome compile` against RadioLib 7.2.1 + ESPHome 2026.6.1, gated by CI
(`.github/workflows/ci.yml`). Every RadioLib call was verified against the pinned
7.2.1 headers; the OTAA/nonce-restore path and the `activateOTAA` return codes are
fixed. Remaining work is hardware-in-the-loop: the live OTAA join, uplink decode,
and power-cycle nonce persistence (spike acceptance #2-#4) need a real TTGO LoRa32
on the gateway.

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
WiFi, the API, and every other component. The current code blocks on purpose
to stay minimal; proving a viable timing model is the spike. **Update:** the
headless deployment decision (no WiFi/API; see *Deployment model*) retires this
for the shipped profile — there is nothing time-sensitive left in the loop to
stall. It only resurfaces if WiFi is ever made co-resident.

**Spike acceptance:**

1. ~~The component builds via `esphome compile`.~~ **Done** — CI-green.
2. On a TTGO LoRa32 v1 (SX1276), it OTAA-joins a real LoRaWAN network
   (US915, sub-band 2) and an uplink is received + decoded server-side. *(pending hardware)*
3. WiFi/API stay responsive during the uplink burst. **Reframed:** the field
   profile is headless (no WiFi/API), so the blocking RX windows have nothing
   time-sensitive to stall — this criterion only bites a WiFi-coresident build,
   which we don't ship. *(see Deployment model)*
4. A **power cycle re-joins without a server-side nonce flush** — i.e. the
   DevNonce persistence works. *(pending hardware)*

With #1 done and #3 reframed, the rest (config polish, SX1262, a compact payload
codec, more regions) is mechanical once #2 and #4 are confirmed on hardware.

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
   exercised band. A wrong channel mask = the device transmits joins on
   channels the gateway never hears = a "won't join" that is not auth.

## What's in the scaffold

```
components/lorawan/
  __init__.py      # config schema (region, sub_band, EUIs/key, radio pins,
                   # uplink_interval) + to_code; pins jgromes/RadioLib 7.2.1,
                   # declares the Arduino SPI library
  sensor.py        # sensor sub-platform: bind an ESPHome sensor as a payload field
  lorawan.h        # LoRaWANComponent: radio + LoRaWANNode, join/uplink, nonce pref
  lorawan.cpp      # setup() inits radio (begin on concrete chip) + joins;
                   # loop() interval-uplinks; nonces saved/restored via ESPPreferences
codec/
  decodeUplink.js  # reference ChirpStack codec (float32-LE, FIELDS in lockstep)
example/
  spike-ttgo-lora32-v1.yaml     # bench config: WiFi + SX1276, keys via !secret
  field-headless.yaml           # production config: LoRaWAN-only, no WiFi/api/ota
  lorawan-secrets.yaml.example  # per-device namespaced keys template
tests/
  ci-compile.yaml  # headless local-source config the CI gate compiles
.github/workflows/
  ci.yml           # esphome compile gate (pinned esphome), on push + PR
README.md  LICENSE (MIT)  .gitignore  CLAUDE.md  HANDOFF.md (this file)
```

Payload encoding (spike): each bound sensor's value is appended as
little-endian `float32` in declaration order. Deliberately trivial — a compact
codec (scaling/ints/bitfields) and a matching server-side `decodeUplink`
generator come later, kept in lockstep with this byte layout.

## Known caveats

- ~~**RadioLib API is version-sensitive / unverified.**~~ **Resolved.** Verified
  against the pinned 7.2.1 headers and builds clean. Fixed: nonce-restore must
  run after `beginOTAA` (which clears nonces) and `setBufferNonces` validates the
  blob checksum against `keyCheckSum`; `activateOTAA` returns `NEW_SESSION` /
  `SESSION_RESTORED` (both negative), not `ERR_NONE`; `begin()` is on the concrete
  chip, not `PhysicalLayer`. Keep building to the pinned version.
- **Blocking `loop()` / `setup()` + Task WDT.** Join and uplink block through the
  RX windows (seconds; longer when no gateway answers). Headless removes the
  WiFi/API stall, but **not** the ESP-IDF Task Watchdog — a multi-second block
  on the loop task reboot-loops the device (observed on hardware: a join with no
  gateway in range trips `task_wdt` ~10 s in). Handled by detaching the loop task
  from the Task WDT around the blocking RadioLib calls (`WdtPause` in
  `lorawan.cpp`). This makes the blocking model safe; the proper fix is still
  async/off-loop radio (tracked with the deep-sleep work). Note `setup()` still
  blocks ~one join attempt (~7 s) on boot when out of range before `loop()` takes
  over retries.
- **SPI ownership** — the component lets RadioLib drive SPI via raw pin numbers
  rather than sharing ESPHome's `spi` bus (and declares the Arduino SPI library
  itself, since ESPHome skips it on ESP32). Fine as-is; revisit if it conflicts
  with other SPI devices on the board.
- **SX1262 path is stubbed** — `init_radio_()` branches on chip but only SX1276
  is exercised. SX1262 needs `dio1` + `busy` (+ usually a TCXO voltage and
  `dio2_as_rf_switch`); add those to the radio schema when you do it.

## Next steps (ordered)

1. ~~**Make it compile.**~~ Done — builds clean against RadioLib 7.2.1 +
   ESPHome 2026.6.1, gated by CI (`.github/workflows/ci.yml` compiles the local
   checkout via `tests/ci-compile.yaml`). Fixed: OTAA nonce-restore ordering,
   the `activateOTAA` session return codes, the concrete-chip `begin()`, and the
   Arduino SPI library dep.
2. **Flash + join.** TTGO LoRa32 v1, real network, US915 sub-band 2. Register
   the device + keys on your server first (MAC version 1.0.4) and flush its
   nonces once.
3. **Verify nonce persistence** — power-cycle, confirm re-join with no server
   flush.
4. **Resolve loop timing.** Largely retired by the deployment decision below:
   field firmware runs with no WiFi/API, so the blocking RX windows have nothing
   time-sensitive to stall (they only delay sensor polling by a couple seconds
   per uplink). Second-core/async is only needed if WiFi is kept co-resident —
   which the field profile does not.
5. Then: SX1262 support, a compact payload codec, and more regions. (Downlink
   RX is done: application downlinks captured from the uplink's RX windows and
   surfaced via the `on_downlink` automation trigger -- port + payload bytes.)

## Deployment model (decided)

Field firmware is **headless: no `wifi:`, no `api:`, no network `ota:`** — serial
`logger:` only. Rationale: LoRaWAN nodes are usually out of WiFi range and on
battery/solar, the keys are compile-time (`!secret`), and ESPHome's `wifi:`/`api:`
both default to `reboot_timeout: 15min` — a configured-but-unreachable WiFi
reboot-loops the device (and every reboot re-joins, burning a DevNonce). So
build → flash → join → update are all **wired (USB serial)**; the join is observed
on the serial log. WiFi only earns its keep on the bench.

### Open considerations

- **Deep sleep (planned; preferred for battery/solar).** The real power lever.
  It wipes RAM, so the model flips from an interval in `loop()` to a one-shot in
  `setup()` then sleep, and the **session** must persist, not just the nonces:

      setup(): init radio -> restore nonces+session -> activateOTAA()
        SESSION_RESTORED -> resume at saved FCnt, no join airtime (steady state)
        NEW_SESSION      -> first boot / session lost: joined fresh
        error            -> backoff, sleep, retry next wake
      read sensor(s) -> uplink (blocking RX) -> save nonces+session
      radio_->sleep() -> global_preferences->sync() -> esp_deep_sleep_start()

  RadioLib `getBufferSession`/`setBufferSession` (size
  `RADIOLIB_LORAWAN_SESSION_BUF_SIZE`) hold DevAddr, session keys, and the frame
  counters; FCntUp must stay monotonic across wakes or the server drops frames as
  replays. Gotchas: (1) `ESPPreferences` batches writes — call
  `global_preferences->sync()` before sleeping or the FCnt is lost; (2) sleep the
  SX127x (`PhysicalLayer::sleep()`, virtual) separately from the ESP or it idles
  at ~1.5 mA; (3) sensors update on their own interval — a one-shot must
  force-read bound sensors and wait before packing the payload, or the first
  cycle uplinks a stale/zero value. Config: `deep_sleep: true` (default false)
  with the uplink interval as the sleep period. Independent of wifi-on-demand;
  the only shared surface is that any Class-A downlink (incl. that feature)
  arrives only in the RX window after a scheduled uplink — up-to-one-interval
  latency.

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

Conventions live in [`CLAUDE.md`](CLAUDE.md). Pin a commit when consuming this
repo elsewhere — never `main`.

## Sources

- ESPHome LoRaWAN tracking: https://github.com/esphome/feature-requests/issues/2634
- RadioLib (LoRaWAN MAC): https://github.com/jgromes/RadioLib
- RadioLib DevNonce-on-power-cycle (TTGO LoRa32): https://github.com/jgromes/RadioLib/issues/1480
- ESPHome external components: https://esphome.io/components/external_components/
- ESP32 eFuse base MAC: https://github.com/espressif/esp-idf/blob/master/components/esp_hw_support/include/esp_mac.h
- arduino-esp32 getEfuseMac byte-order issue: https://github.com/espressif/arduino-esp32/issues/6458
