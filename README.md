# lorawan-for-esphome

A LoRaWAN MAC component for [ESPHome](https://esphome.io). It joins a LoRaWAN
network over OTAA and uplinks ESPHome sensor values, using
[RadioLib](https://github.com/jgromes/RadioLib) for the radio + MAC and
ESPHome's own SPI, sensor, and preference machinery for everything else.

ESPHome ships raw LoRa radio components (`sx126x`, `sx127x`) but no LoRaWAN
MAC. This fills that gap so any ESPHome sensor platform can be the source of a
LoRaWAN uplink — no per-sensor firmware code.

> **Status: spike.** This is the minimal slice that proves the hard part —
> joining a real network and surviving a power cycle without a server-side
> nonce flush — works inside ESPHome's cooperative loop. It blocks in `loop()`
> through the LoRaWAN RX windows on purpose; making that non-blocking (or
> second-core) is the next step. Not yet hardware-validated. Pin a commit, not
> `main`.

## Usage

```yaml
external_components:
  - source: github://moellere/lorawan-for-esphome
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
in declaration order. The matching ChirpStack `decodeUplink` codec is generated
from the same ordered list (in the consuming project) so device and server stay
in lockstep. The encoding is deliberately trivial for the spike; a compact
codec (scaling, integers, bitfields) is a later refinement.

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

## Region support

Only **US915 sub-band 2** has been exercised. The config schema accepts other
regions so the band table can grow without a schema change, but they are
unverified.

## License

MIT — see [LICENSE](LICENSE).
