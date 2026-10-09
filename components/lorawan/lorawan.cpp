#include "lorawan.h"
#include "esphome/core/log.h"
#include "esphome/core/hal.h"

#include <SPI.h>
#include <esp_task_wdt.h>

namespace esphome {
namespace lorawan {

static const char *const TAG = "lorawan";

namespace {
// RadioLib's join and sendReceive block through the LoRaWAN RX windows (seconds,
// and longer when no gateway answers) -- past the Task WDT period, which
// otherwise reboot-loops the device mid-join. Detach the calling task from the
// Task WDT for the duration of the blocking call, then re-attach.
struct WdtPause {
  WdtPause() { esp_task_wdt_delete(nullptr); }
  ~WdtPause() { esp_task_wdt_add(nullptr); }
};

// Lowest US915 data rate whose maximum application payload still fits `size`.
// DR0 carries 11 bytes, DR1 53, DR2 125; DR3 and up carry more but cost range,
// so this returns the slowest rate that works rather than the fastest.
uint8_t min_datarate_for(size_t size) {
  if (size <= 11)
    return 0;
  if (size <= 53)
    return 1;
  return 3;
}
}  // namespace

// RadioLib's nonce buffer is a fixed size; persist exactly that blob in NVS.
struct NoncesBlob {
  uint8_t data[RADIOLIB_LORAWAN_NONCES_BUF_SIZE];
};

static uint64_t parse_hex_u64(const std::string &hex) {
  return (uint64_t) strtoull(hex.c_str(), nullptr, 16);
}

void LoRaWANComponent::set_credentials(const std::string &join_eui, const std::string &dev_eui,
                                       const std::string &app_key) {
  this->join_eui_ = parse_hex_u64(join_eui);
  this->dev_eui_ = parse_hex_u64(dev_eui);
  for (size_t i = 0; i < 16 && (i * 2 + 1) < app_key.size(); i++) {
    this->app_key_[i] = (uint8_t) strtoul(app_key.substr(i * 2, 2).c_str(), nullptr, 16);
  }
}

bool LoRaWANComponent::init_radio_() {
  // Front-end power/enable first: a board with an external PA (Heltec V3/V4)
  // has its RF path unpowered at reset, and every later step still "succeeds"
  // -- begin() returns OK, uplinks report sent, and nothing ever reaches the
  // gateway. Assert these before anything touches the radio.
  for (int pin : this->setup_high_pins_) {
    pinMode(pin, OUTPUT);
    digitalWrite(pin, HIGH);
    ESP_LOGD(TAG, "front-end pin %d driven high", pin);
  }
  // Active-low rails (e.g. Heltec V4's VEXT on GPIO36, which powers the FEM /
  // antenna path): an undriven VEXT leaves the front end unpowered -- TX still
  // "works" as leakage at desk range while RX is completely deaf, which is an
  // expensive failure to see from the device side.
  for (int pin : this->setup_low_pins_) {
    pinMode(pin, OUTPUT);
    digitalWrite(pin, LOW);
    ESP_LOGD(TAG, "front-end pin %d driven low", pin);
  }

  // Bind the Arduino SPI bus to the configured pins before RadioLib constructs
  // the Module. RadioLib otherwise defaults to arduino-esp32's VSPI pins
  // (18/19/23/5), which match almost no LoRa board's radio wiring and surface as
  // ERR_CHIP_NOT_FOUND. Only when all three were supplied (schema enforces that).
  if (this->sck_pin_ >= 0) {
    SPI.begin(this->sck_pin_, this->miso_pin_, this->mosi_pin_, this->cs_pin_);
  }

  Module *mod = new Module(this->cs_pin_, this->irq_pin_, this->rst_pin_, this->busy_pin_);
  // Per-transfer RF switching (PA/LNA enables). RadioLib owns these pins from
  // here: idle LOW, txen HIGH only while transmitting, rxen HIGH only while
  // receiving. A pin listed here must not also be in setup_high.
  if (this->rxen_pin_ >= 0 || this->txen_pin_ >= 0) {
    mod->setRfSwitchPins(this->rxen_pin_ >= 0 ? (uint32_t) this->rxen_pin_ : RADIOLIB_NC,
                         this->txen_pin_ >= 0 ? (uint32_t) this->txen_pin_ : RADIOLIB_NC);
    ESP_LOGD(TAG, "rf switch pins: rxen=%d txen=%d", this->rxen_pin_, this->txen_pin_);
  }
  // begin() lives on the concrete radio, not PhysicalLayer, and its frequency
  // args are placeholders -- LoRaWANNode reprograms the channel per uplink.
  int16_t state;
  if (this->chip_ == "sx1276") {
    auto *radio = new SX1276(mod);
    state = radio->begin();
    this->radio_ = radio;
  } else if (this->chip_ == "sx1278") {
    auto *radio = new SX1278(mod);
    state = radio->begin();
    this->radio_ = radio;
  } else if (this->chip_ == "sx1262") {
    auto *radio = new SX1262(mod);
    // The TCXO voltage has to go in at begin(): the SX1262 powers its
    // oscillator from DIO3, and if that is wrong the chip never clocks and
    // begin() fails ERR_SPI_CMD_TIMEOUT, which reads like miswired SPI.
    // RadioLib's own default is 1.6 V, so only override when configured.
    if (this->tcxo_voltage_ >= 0.0f) {
      state = radio->begin(434.0, 125.0, 9, 7, RADIOLIB_SX126X_SYNC_WORD_PRIVATE, 10, 8,
                           this->tcxo_voltage_);
      ESP_LOGD(TAG, "sx1262 begin with tcxo %.2fV", this->tcxo_voltage_);
    } else {
      state = radio->begin();
    }
    if (state == RADIOLIB_ERR_NONE && this->dio2_as_rf_switch_) {
      // Antenna switch driven from DIO2 rather than a GPIO. Without this the
      // PA transmits into a switch stuck in receive.
      int16_t rf = radio->setDio2AsRfSwitch(true);
      if (rf != RADIOLIB_ERR_NONE) {
        ESP_LOGE(TAG, "setDio2AsRfSwitch failed: %d", rf);
        return false;
      }
    }
    this->radio_ = radio;
  } else {
    ESP_LOGE(TAG, "unknown radio chip '%s'", this->chip_.c_str());
    return false;
  }
  if (state != RADIOLIB_ERR_NONE) {
    ESP_LOGE(TAG, "radio begin failed: %d", state);
    return false;
  }

  // US915 is the only band exercised in the spike; the schema accepts others so
  // this switch can grow without a config change.
  const LoRaWANBand_t *band = &US915;
  this->node_ = new LoRaWANNode(this->radio_, band, this->sub_band_);
  return true;
}

bool LoRaWANComponent::restore_nonces_() {
  NoncesBlob blob{};
  if (!this->nonces_pref_.load(&blob))
    return false;
  // setBufferNonces validates the blob's checksum against keyCheckSum, which is
  // only set by beginOTAA -- so this must run after beginOTAA, not before.
  return this->node_->setBufferNonces(blob.data) == RADIOLIB_ERR_NONE;
}

void LoRaWANComponent::save_nonces_() {
  NoncesBlob blob{};
  memcpy(blob.data, this->node_->getBufferNonces(), sizeof(blob.data));
  this->nonces_pref_.save(&blob);
}

bool LoRaWANComponent::join_() {
  this->node_->beginOTAA(this->join_eui_, this->dev_eui_, nullptr, this->app_key_);
  // beginOTAA calls clearNonces(); restore the persisted DevNonce afterwards so
  // it stays monotonic across reboots, or the server drops the join silently.
  this->restore_nonces_();
  // Blocks through the join RX windows. See the class comment in lorawan.h.
  int16_t state;
  {
    WdtPause wdt_pause;
    state = this->node_->activateOTAA();
  }
  this->save_nonces_();  // persist the new DevNonce regardless of outcome
  if (state == RADIOLIB_LORAWAN_NEW_SESSION || state == RADIOLIB_LORAWAN_SESSION_RESTORED) {
    ESP_LOGI(TAG, "OTAA join OK (%s)",
             state == RADIOLIB_LORAWAN_SESSION_RESTORED ? "restored" : "new session");
    if (this->device_class_ == "C") {
      // LoRaWAN 1.0.x switches immediately; 1.1 queues DeviceModeInd on the
      // next uplink. Either way RxC opens after the first post-join uplink.
      int16_t cs = this->node_->setClass(RADIOLIB_LORAWAN_CLASS_C);
      if (cs == RADIOLIB_ERR_NONE)
        ESP_LOGI(TAG, "device class C enabled");
      else
        ESP_LOGW(TAG, "setClass(C) failed: %d", cs);
    }
    return true;
  }
  ESP_LOGW(TAG, "OTAA join failed: %d", state);
  return false;
}

void LoRaWANComponent::dispatch_downlink_(uint8_t f_port, const uint8_t *data, size_t len) {
  std::vector<uint8_t> down_payload(data, data + len);
  for (auto *t : this->downlink_triggers_)
    t->trigger(f_port, down_payload);
}

bool LoRaWANComponent::transmit_(const uint8_t *data, size_t len, uint8_t f_port) {
  // Re-assert the data rate this payload needs, every time. ADR drives the
  // rate down on a strong link, and US915 DR0 caps the application payload at
  // 11 bytes -- so a 12-byte payload (three float32s) joins fine, uplinks once
  // or twice, then fails with RADIOLIB_ERR_PACKET_TOO_LONG (-4) forever once
  // the network has settled the rate. Setting it once after join is not enough
  // because ADR lowers it again afterwards.
  this->node_->setDatarate(min_datarate_for(len));

  // Capture any downlink that lands in RX1/RX2. RadioLib ignores the incoming
  // lenDown (it zeroes it) and writes up to MAX_PAYLOAD_SIZE bytes, so the
  // buffer must be at least that. Blocks through the RX windows — the timing
  // risk this spike exists to measure.
  uint8_t down[RADIOLIB_LORAWAN_MAX_PAYLOAD_SIZE];
  size_t down_len = 0;
  LoRaWANEvent_t down_event{};
  int16_t state;
  {
    WdtPause wdt_pause;
    state = this->node_->sendReceive(data, len, f_port, down, &down_len, false,
                                     nullptr, &down_event);
  }
  this->save_nonces_();
  if (state < RADIOLIB_ERR_NONE) {
    ESP_LOGW(TAG, "uplink failed: %d", state);
    return false;
  }
  // The DevEUI rides along because it is the only thing that identifies this
  // board to the network server, and nothing else emits it after boot. A
  // fleet tool reading a window of recent serial can then ask "who is in this
  // slot" without power-cycling the board to catch dump_config.
  ESP_LOGD(TAG, "uplink sent (%u bytes, fport=%u) dev_eui=%016llx",
           (unsigned) len, f_port, (unsigned long long) this->dev_eui_);
  // state is the RX window (1 or 2) when a downlink arrived, 0 when none.
  if (state > 0 && down_len > 0) {
    ESP_LOGD(TAG, "downlink fport=%u (%u bytes)%s", down_event.fPort, (unsigned) down_len,
             down_event.frmPending ? ", more pending" : "");
    this->dispatch_downlink_(down_event.fPort, down, down_len);
  }
  return true;
}

void LoRaWANComponent::uplink_() {
  std::vector<uint8_t> payload;
  payload.reserve(this->fields_.size() * 4);
  for (auto *s : this->fields_) {
    float v = s->state;
    uint8_t *b = reinterpret_cast<uint8_t *>(&v);
    payload.insert(payload.end(), b, b + 4);  // float32 little-endian, see codec
  }
  this->transmit_(payload.data(), payload.size(), 1);
}

bool LoRaWANComponent::send_raw(uint8_t f_port, const std::vector<uint8_t> &payload) {
  if (!this->joined_) {
    ESP_LOGW(TAG, "send_raw dropped: not joined");
    return false;
  }
  // An explicit send counts as the uplink for interval purposes, so the
  // periodic sensor pack (if any) does not pile on right behind it.
  this->last_uplink_ = millis();
  return this->transmit_(payload.data(), payload.size(), f_port);
}

// Class C: RadioLib re-arms continuous RX after every uplink and latches
// arriving frames from the radio IRQ; this poll just parses whatever latched.
// Cheap when idle (a bool check), so it runs every loop() pass.
void LoRaWANComponent::poll_class_c_() {
  uint8_t down[RADIOLIB_LORAWAN_MAX_PAYLOAD_SIZE];
  size_t down_len = 0;
  LoRaWANEvent_t down_event{};
  int16_t state = this->node_->getDownlinkClassC(down, &down_len, &down_event);
  if (state > 0 && down_len > 0) {
    ESP_LOGD(TAG, "class C downlink fport=%u (%u bytes)", down_event.fPort, (unsigned) down_len);
    this->dispatch_downlink_(down_event.fPort, down, down_len);
  } else if (state < RADIOLIB_ERR_NONE) {
    ESP_LOGW(TAG, "class C receive failed: %d", state);
  }
}

void LoRaWANComponent::setup() {
  ESP_LOGCONFIG(TAG, "setting up LoRaWAN...");
  if (!this->init_radio_()) {
    this->mark_failed();
    return;
  }
  // Key carries RadioLib's nonce-buffer version (NONCES_VERSION_VAL). 7.7 dropped
  // a byte from the layout, so a blob saved under an older RadioLib must never
  // reach setBufferNonces; a new key makes the old one unreachable.
  this->nonces_pref_ = global_preferences->make_preference<NoncesBlob>(fnv1_hash("lorawan_nonces_v3"));
  this->joined_ = this->join_();
  if (!this->joined_)
    this->status_set_warning();
}

void LoRaWANComponent::loop() {
  if (!this->joined_) {
    // Retry join no more often than the uplink interval to respect duty cycle.
    if (millis() - this->last_uplink_ < this->uplink_interval_ms_)
      return;
    this->last_uplink_ = millis();
    this->joined_ = this->join_();
    if (this->joined_)
      this->status_clear_warning();
    return;
  }
  if (this->device_class_ == "C")
    this->poll_class_c_();
  if (millis() - this->last_uplink_ < this->uplink_interval_ms_)
    return;
  this->last_uplink_ = millis();
  // No sensor fields configured means the payload is application-driven
  // (lorawan.send_raw); don't emit empty periodic uplinks on top of it.
  if (!this->fields_.empty())
    this->uplink_();
}

void LoRaWANComponent::dump_config() {
  ESP_LOGCONFIG(TAG, "LoRaWAN:");
  ESP_LOGCONFIG(TAG, "  chip: %s", this->chip_.c_str());
  ESP_LOGCONFIG(TAG, "  region: %s  sub_band: %u", this->region_.c_str(), this->sub_band_);
  ESP_LOGCONFIG(TAG, "  pins: cs=%d rst=%d dio/irq=%d busy=%d sck=%d miso=%d mosi=%d",
                this->cs_pin_, this->rst_pin_, this->irq_pin_, this->busy_pin_,
                this->sck_pin_, this->miso_pin_, this->mosi_pin_);
  ESP_LOGCONFIG(TAG, "  dev_eui: %016llx", (unsigned long long) this->dev_eui_);
  ESP_LOGCONFIG(TAG, "  device_class: %s", this->device_class_.c_str());
  ESP_LOGCONFIG(TAG, "  uplink_interval: %u ms", this->uplink_interval_ms_);
  ESP_LOGCONFIG(TAG, "  payload fields: %u", (unsigned) this->fields_.size());
}

}  // namespace lorawan
}  // namespace esphome
