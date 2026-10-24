# lorawan-for-esphome

[![CI](https://github.com/moellere/lorawan-for-esphome/actions/workflows/ci.yml/badge.svg)](https://github.com/moellere/lorawan-for-esphome/actions/workflows/ci.yml)

A LoRaWAN MAC component for [ESPHome](https://esphome.io). It joins a LoRaWAN
network over OTAA and uplinks ESPHome sensor values, using
[RadioLib](https://github.com/jgromes/RadioLib) for the radio + MAC and
ESPHome's own SPI, sensor, and preference machinery for everything else.

ESPHome ships raw LoRa radio components (`sx126x`, `sx127x`) but no LoRaWAN
MAC. This fills that gap so any ESPHome sensor platform can be the source of a
LoRaWAN uplink — no per-sensor firmware code.

> **Status: pre-alpha.** Compiles cleanly (CI-gated) against RadioLib 7.7.1 +
> ESPHome 2026.6.1. On hardware (Heltec V4 / SX1262, US915 sub-band 2) it has
> OTAA-joined, uplinked with an advancing FCnt, and received a downlink through
> `on_downlink`, on RadioLib 7.2.1 (2026-08-28) and again on `main` @ ca52396
> with RadioLib 7.7.1 and the Task WDT change (2026-10-24; see HANDOFF.md, *HIL
> findings*). A true power-cycle nonce check is still pending. RadioLib's
> blocking join and uplinks run on their own FreeRTOS task, so ESPHome's loop
> keeps running through the RX windows; that radio task has not yet run on
> hardware. Give the radio its own SPI pins: nothing else may share its bus.
> Pin a commit, not `main`.

## Usage

```yaml
esp32:
  board: ttgo-lora32-v1   # Arduino or ESP-IDF framework both work

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

# The radio is an ESPHome SPI device, and its bus is its own: the component's
# radio task drives it alongside the main loop, so the schema rejects any other
# SPI device on the same spi: bus.
spi:
  clk_pin: GPIO5
  miso_pin: GPIO19
  mosi_pin: GPIO27

sensor:
  - platform: lorawan
    lorawan_id: lw
    sensor: some_existing_sensor   # appended to the uplink, float32 LE by default
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

Each `sensor:` binding appends its current value to the uplink (fPort 1),
little-endian, in declaration order. `type` sets the encoding; the default is
`float32`. The integer types send `round(value * multiplier)`:

```yaml
sensor:
  - platform: lorawan
    lorawan_id: lw
    sensor: temperature
    type: int16          # 2 bytes
    multiplier: 100      # 21.37 C is sent as 2137
```

| `type` | Bytes | Sent value range | No reading |
|---|---|---|---|
| `float32` | 4 | the raw value | `NaN` |
| `int8` / `uint8` | 1 | -127..127 / 0..254 | -128 / 255 |
| `int16` / `uint16` | 2 | -32767..32767 / 0..65534 | -32768 / 65535 |
| `int32` / `uint32` | 4 | ±2147483647 / 0..4294967294 | min / max |

- `multiplier` applies to the integer types only (default 1) and must be
  nonzero. Pick it for the resolution you need, then the smallest type whose
  range covers `value * multiplier`.
- A value out of range saturates at the range's edge, never wraps.
- A sensor with no reading yet (`NaN`) sends the type's "no reading" code,
  which no real value takes.

A matching ChirpStack `decodeUplink` codec is in
[`codec/decodeUplink.js`](codec/decodeUplink.js). List the same fields there,
in order, with the same `type` and `multiplier`; it returns the "no reading"
code as `null` and rejects a payload of the wrong length. CI packs test values
with the firmware's own encoder and decodes them with that file, so the two
cannot drift apart.

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
    type: uint16
    multiplier: 1000       # millivolts
```

**Payload size vs data rate.** US915 at the slowest data rate (DR0/SF10) caps
the application payload at **11 bytes**: two `float32` fields, or five
`int16`. Each uplink takes the slowest rate that fits (see *Region support*);
a payload too big for the fastest rate the link allows is dropped. The config
dump logs the payload size. Values are read at uplink time, so keep each
sensor's `update_interval` at or below `uplink_interval` (the first post-boot
uplink may carry "no reading" if a slow sensor hasn't reported yet). Bindings
take numeric `sensor::Sensor` values only. For a layout this cannot express
(bitfields, a different fPort), build the bytes in a lambda and use
`lorawan.send_raw`.

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
the device — and every reboot before a session is stored re-joins, burning a
DevNonce. Omit those components (or set their `reboot_timeout: 0s`).

Workflow — build, flash, and update are all **wired (USB serial)**:

```bash
esphome run example/field-headless.yaml --device /dev/ttyUSB0
```

Watch the serial log for `OTAA join OK` and `uplink sent`. Both the config dump
and every `uplink sent` line carry `dev_eui`, so a board can be asked which
device it is without a reboot -- the DevEUI is an identifier, not a secret, and
it is the only thing tying the board in front of you to a record on the network
server. Register the device and
flush its DevNonces on the server once before first join; on later re-flashes,
write the **app region only** to preserve the NVS-stored nonces and session (a
full erase resets them). With a stored session, a reboot or re-flash logs
`OTAA join OK (restored)` and resumes at the saved frame counter with no join. Future low-power (deep sleep) and remote-update (WiFi-on-demand via
downlink) directions are tracked in [HANDOFF.md](HANDOFF.md).

### Deep sleep (battery)

Point `lorawan` at an ESPHome `deep_sleep` and each wake does one uplink:

```yaml
deep_sleep:
  id: deep_sleep_1
  sleep_duration: 15min     # the uplink cadence

lorawan:
  # ...
  deep_sleep_id: deep_sleep_1
```

Each wake resumes the stored session (or joins, the first time), waits up to
10 s for the bound sensors to report, uplinks once through the RX windows, puts
the radio to sleep, and hands over to `deep_sleep`. The rules:

- `sleep_duration` is required and sets the cadence; `uplink_interval` is
  ignored. `run_duration` is rejected: it could sleep the device mid-join.
- Class A only; `device_class: C` is rejected.
- A failed join just sleeps; the next wake retries, so join attempts are spaced
  by `sleep_duration`.
- Downlinks arrive only in that one uplink's RX windows, and the device sleeps
  right after, so an `on_downlink` action with a `delay` will not finish.
- The payload is the bound sensors. A sensor that needs a long warm-up (longer
  than the 10 s wait) is uplinked as `NaN`, with a warning in the log.
- `deep_sleep.prevent` still holds the device awake, e.g. for a maintenance
  window.

Deep sleep only helps as much as the board lets it: the TTGO LoRa32's
regulator and power LED draw milliamps regardless (see HANDOFF.md).

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
  and the session survive → a re-flash resumes the session with no join.
- A full **"Erase device"** wipes NVS → DevNonces reset → the next join is
  silently dropped unless paired with a **server-side nonce flush**.
- First flash of a blank device: register it and flush nonces once; no erase
  concern (NVS is empty anyway).
- A firmware that **changes RadioLib's nonce-buffer version** also resets the
  DevNonces, even without an erase: the old blob is no longer readable. The
  7.2.1 -> 7.7.1 bump is one; flush server-side once after flashing it.
- **Resetting the device on the server** (deleting or re-registering it, new
  keys) while the board keeps its NVS leaves it uplinking on a session the
  server no longer knows, and it never re-joins on its own. Pair a server-side
  reset with a full erase so the board joins fresh. Changing the keys in the
  firmware is safe: the stored nonces and session no longer match and are
  discarded.

So configure the webflasher to **not full-erase on re-flash** (preserve nonces),
or flush server-side when you do. A shared-binary fleet (one image for many
devices) is the alternative — it would need runtime key provisioning over serial,
which is not implemented; local build avoids it.

## Region support

| `region` | `sub_band` | Run on hardware |
|---|---|---|
| `US915` | 1–8 (default 2) | yes, sub-band 2 |
| `AU915` | 1–8 (default 2) | no |
| `EU868` | not used (rejected) | no |

The bands are RadioLib's own tables, and the uplink data rate comes from them:
each uplink takes the slowest rate whose payload limit (after dwell time and
any queued MAC answers) fits. `AU915` and `EU868` compile and validate but have
never transmitted; the config validation and the boot log both warn. Testing
one needs a gateway on that region's plan, and transmitting `EU868` outside a
region that allows it is not legal. Any other region is rejected.

The component runs on **ESP32** under either the Arduino or the ESP-IDF
framework: RadioLib talks to the radio through ESPHome's own SPI and GPIO.

**Upgrading from a config with `sck_pin`/`miso_pin`/`mosi_pin` under
`radio:`:** move them to a top-level `spi:` block as `clk_pin`, `miso_pin` and
`mosi_pin`; `cs_pin` stays under `radio:`. The old keys are rejected with that
message.

## License

MIT — see [LICENSE](LICENSE).
