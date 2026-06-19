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

**Spike, not hardware-validated.** The scaffold compiles in intent but has
**not been built against ESPHome or RadioLib yet** — treat the C++ as a first
draft against a version-sensitive API (see *Known caveats*). The job of the
first work session is to make it build and then join a real network.

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
to stay minimal; proving a viable timing model is the spike.

**Spike acceptance:**

1. The component builds via `esphome compile` against the example config.
2. On a TTGO LoRa32 v1 (SX1276), it OTAA-joins a real LoRaWAN network
   (US915, sub-band 2) and an uplink is received + decoded server-side.
3. WiFi/API stay responsive during the uplink burst (or the radio is moved
   off the main loop — second core / async — if blocking proves unacceptable).
4. A **power cycle re-joins without a server-side nonce flush** — i.e. the
   DevNonce persistence works.

If all four hold, the rest (config polish, SX1262, a compact payload codec,
more regions) is mechanical.

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
                   # uplink_interval) + to_code; pins jgromes/RadioLib 7.2.1
  sensor.py        # sensor sub-platform: bind an ESPHome sensor as a payload field
  lorawan.h        # LoRaWANComponent: radio + LoRaWANNode, join/uplink, nonce pref
  lorawan.cpp      # setup() inits radio + joins; loop() interval-uplinks;
                   # nonces saved/restored via ESPPreferences
example/
  spike-ttgo-lora32-v1.yaml   # SX1276 spike config (keys via !secret)
README.md  LICENSE (MIT)  .gitignore  CLAUDE.md  HANDOFF.md (this file)
```

Payload encoding (spike): each bound sensor's value is appended as
little-endian `float32` in declaration order. Deliberately trivial — a compact
codec (scaling/ints/bitfields) and a matching server-side `decodeUplink`
generator come later, kept in lockstep with this byte layout.

## Known caveats in the scaffold (fix these first)

- **RadioLib API is version-sensitive.** `beginOTAA` / `activateOTAA` /
  `sendReceive` / `getBufferNonces` / `setBufferNonces` signatures and the
  `RADIOLIB_*` / `US915` / `LoRaWANBand_t` symbols vary across RadioLib 6.x↔7.x.
  The code targets 7.x but is unverified — expect to adjust against the pinned
  version until `esphome compile` is clean. Pin one RadioLib version and build
  to it.
- **Blocking `loop()`** — see the spike goal. Acceptable for the spike;
  resolve before "done."
- **SPI ownership** — the component lets RadioLib drive SPI via raw pin
  numbers rather than sharing ESPHome's `spi` bus. Fine for the spike; revisit
  if it conflicts with other SPI devices on the board.
- **SX1262 path is stubbed** — `init_radio_()` branches on chip but only
  SX1276 is exercised. SX1262 needs `dio1` + `busy` (+ usually a TCXO voltage
  and `dio2_as_rf_switch`); add those to the radio schema when you do it.

## Next steps (ordered)

1. **Make it compile.** `esphome compile example/spike-ttgo-lora32-v1.yaml`
   (with a `secrets.yaml`). Fix RadioLib API mismatches against the pinned
   version. This is the bulk of session one.
2. **Flash + join.** TTGO LoRa32 v1, real network, US915 sub-band 2. Register
   the device + keys on your server first (MAC version 1.0.4) and flush its
   nonces once.
3. **Verify nonce persistence** — power-cycle, confirm re-join with no server
   flush.
4. **Resolve loop timing** if blocking is unacceptable (measure WiFi/API
   responsiveness first; only go second-core/async if needed).
5. Then: SX1262 support, a compact payload codec, downlink handling, more
   regions, and a CI `esphome compile` gate (the component must be fetchable
   via `external_components` with a pinned ref).

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
