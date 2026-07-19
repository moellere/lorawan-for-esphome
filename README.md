# lorawan-for-esphome

[![CI](https://github.com/moellere/lorawan-for-esphome/actions/workflows/ci.yml/badge.svg)](https://github.com/moellere/lorawan-for-esphome/actions/workflows/ci.yml)

A LoRaWAN MAC component for [ESPHome](https://esphome.io). It joins a LoRaWAN
network over OTAA and uplinks ESPHome sensor values, using
[RadioLib](https://github.com/jgromes/RadioLib) for the radio + MAC and
ESPHome's own SPI, sensor, and preference machinery for everything else.

ESPHome ships raw LoRa radio components (`sx126x`, `sx127x`) but no LoRaWAN
MAC. This fills that gap so any ESPHome sensor platform can be the source of a
LoRaWAN uplink — no per-sensor firmware code.

> **Status: pre-alpha.** Compiles cleanly (CI-gated) against RadioLib 7.2.1 +
> ESPHome 2026.6.1, with the OTAA/nonce-persistence path verified against the
> pinned RadioLib API. **Not yet hardware-validated** — the live OTAA join is
> still pending. The field profile runs headless (no WiFi), which sidesteps the
> RX-window/loop-timing concern: `loop()` blocks through the RX windows, but
> with no WiFi/API there is nothing time-sensitive to stall. Pin a commit, not
> `main`.

## Usage

```yaml
external_components:
  - source: github://moellere/lorawan-for-esphome@<commit-or-tag>  # pin, not main
    components: [lorawan]

lorawan:
  id: lw
  region: US915
  sub_band: 2
  dev_eui: !secret dev_eui
  join_eui: !secret join_eui
  app_key: !secret app_key
  uplink_interval: 5min
  radio:
    chip: sx1276          # sx1276 | sx1278 | sx1262
    cs_pin: GPIO18
    rst_pin: GPIO23
    dio0_pin: GPIO26      # SX126x uses dio1_pin + busy_pin instead

sensor:
  - platform: lorawan
    lorawan_id: lw
    sensor: some_existing_sensor   # appended to the uplink as float32 LE
```

See [`example/spike-ttgo-lora32-v1.yaml`](example/spike-ttgo-lora32-v1.yaml)
for a complete spike config.

## Per-device keys

`dev_eui` / `join_eui` / `app_key` are per-device registration secrets. ESPHome's
`!secret` only reads a file named `secrets.yaml`, so to keep the LoRaWAN keys in
their own file, merge-include it from the main one:

```yaml
# secrets.yaml
<<: !include lorawan-secrets.yaml
wifi_ssid: "..."
wifi_password: "..."
```

```yaml
# lorawan-secrets.yaml  (gitignored; namespace each device with a <node>_ prefix)
kitchen_dev_eui:  "70b3d5..."
kitchen_join_eui: "0000000000000000"
kitchen_app_key:  "abcd...ef"
```

```yaml
# kitchen.yaml
lorawan:
  dev_eui: !secret kitchen_dev_eui
  join_eui: !secret kitchen_join_eui
  app_key: !secret kitchen_app_key
```

Keys are MSB hex (matches the ChirpStack UI). Substitutions cannot be used inside
`!secret`, so the `<node>_` prefix is spelled out per device. See
[`example/lorawan-secrets.yaml.example`](example/lorawan-secrets.yaml.example).

## Payload encoding

Each `sensor:` binding appends its current value as a little-endian `float32`,
in declaration order. A matching ChirpStack `decodeUplink` codec is in
[`codec/decodeUplink.js`](codec/decodeUplink.js) — keep its `FIELDS` list in
lockstep with the sensor bindings so device and server agree on the byte layout.
The encoding is deliberately trivial for now; a compact codec (scaling, integers,
bitfields) is a later refinement.

To send several values, bind each sensor in payload order — see
[`example/multi-sensor.yaml`](example/multi-sensor.yaml):

```yaml
sensor:
  - platform: adc          # a real sensor (any ESPHome sensor platform)
    pin: GPIO36
    id: battery
  - platform: lorawan      # bind it into the uplink
    lorawan_id: lw
    sensor: battery
```

**Payload size vs data rate.** Each field is 4 bytes, and US915 at the slowest
data rate (DR0/SF10) caps the application payload near **11 bytes** — so only
**2** `float32` fields fit at DR0; more needs a higher DR (better link) or the
frame is dropped. Values are read at uplink time, so keep each sensor's
`update_interval` at or below `uplink_interval` (the first post-boot uplink may
carry `NaN` if a slow sensor hasn't reported yet). Bindings take numeric
`sensor::Sensor` values only.

## Receiving downlinks

Each uplink opens the Class-A RX1/RX2 windows; an application downlink that lands
there fires the `on_downlink` automation with the downlink's `port` (uint8) and
`payload` (`std::vector<uint8_t>`):

```yaml
lorawan:
  id: lw
  # ...
  on_downlink:
    then:
      - lambda: |-
          ESP_LOGI("app", "downlink fport=%u len=%u", port, payload.size());
          if (port == 10 && payload.size() == 1)
            id(relay).turn_on();
```

The component stays generic — what a downlink *means* is yours to define in the
handler (the mirror of the uplink codec, encoded server-side). MAC downlinks
(ADR, link-check) are handled by RadioLib and don't reach the trigger.

**Class-A latency.** A downlink can only be received in the window right after an
uplink, so a server-queued downlink waits for the **next uplink** — up to one
`uplink_interval` of latency (longer with deep sleep). Confirmed downlinks are
auto-ACKed on the following uplink.

## Deployment

A field LoRaWAN node is usually out of WiFi range and on battery/solar, so the
production profile is **headless: no `wifi:`, no `api:`, no network `ota:`** —
serial `logger:` only. See [`example/field-headless.yaml`](example/field-headless.yaml);
[`example/spike-ttgo-lora32-v1.yaml`](example/spike-ttgo-lora32-v1.yaml) keeps WiFi
and is a bench/observation config.

Do not just leave WiFi configured and let it fail: ESPHome's `wifi:` and `api:`
both default to `reboot_timeout: 15min`, so an unreachable network reboot-loops
the device — and every reboot re-joins, burning a DevNonce. Omit those components
(or set their `reboot_timeout: 0s`).

Workflow — build, flash, and update are all **wired (USB serial)**:

```bash
esphome run example/field-headless.yaml --device /dev/ttyUSB0
```

Watch the serial log for `OTAA join OK` and `uplink sent`. Register the device and
flush its DevNonces on the server once before first join; on later re-flashes,
write the **app region only** to preserve the NVS-stored nonces (a full erase
resets them). Future low-power (deep sleep) and remote-update (WiFi-on-demand via
downlink) directions are tracked in [HANDOFF.md](HANDOFF.md).

### Webflashing and per-device builds (local build)

Because these devices have no WiFi/OTA, the fleet model is **local build**: the
build host compiles a *per-device* binary with that device's keys baked in (the
per-device `!secret` keys above are exactly this input), and a browser webflasher
([ESP Web Tools](https://esphome.github.io/esp-web-tools/), WebSerial over USB)
flashes it. The device half needs nothing beyond what's here — keys are
compile-time and `esphome compile` emits a merged `firmware.factory.bin` plus a
manifest for the webflasher. Building those and serving them is the orchestrator's
job (server half), not this repo.

The one device-half contract the webflasher must honor is the **NVS / DevNonce**
rule (same as the wired "app region only" point above):

- Flashing the firmware parts **without a full erase preserves NVS** → DevNonces
  survive → a re-flash re-joins cleanly.
- A full **"Erase device"** wipes NVS → DevNonces reset → the next join is
  silently dropped unless paired with a **server-side nonce flush**.
- First flash of a blank device: register it and flush nonces once; no erase
  concern (NVS is empty anyway).

So configure the webflasher to **not full-erase on re-flash** (preserve nonces),
or flush server-side when you do. A shared-binary fleet (one image for many
devices) is the alternative — it would need runtime key provisioning over serial,
which is not implemented; local build avoids it.

## Region support

Only **US915 sub-band 2** has been exercised. The config schema accepts other
regions so the band table can grow without a schema change, but they are
unverified.

## License

MIT — see [LICENSE](LICENSE).
