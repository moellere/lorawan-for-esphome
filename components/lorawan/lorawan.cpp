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
  // Bind the Arduino SPI bus to the configured pins before RadioLib constructs
  // the Module. RadioLib otherwise defaults to arduino-esp32's VSPI pins
  // (18/19/23/5), which match almost no LoRa board's radio wiring and surface as
  // ERR_CHIP_NOT_FOUND. Only when all three were supplied (schema enforces that).
  if (this->sck_pin_ >= 0) {
    SPI.begin(this->sck_pin_, this->miso_pin_, this->mosi_pin_, this->cs_pin_);
  }

  Module *mod = new Module(this->cs_pin_, this->irq_pin_, this->rst_pin_, this->busy_pin_);
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
    state = radio->begin();
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
    return true;
  }
  ESP_LOGW(TAG, "OTAA join failed: %d", state);
  return false;
}

void LoRaWANComponent::uplink_() {
  std::vector<uint8_t> payload;
  payload.reserve(this->fields_.size() * 4);
  for (auto *s : this->fields_) {
    float v = s->state;
    uint8_t *b = reinterpret_cast<uint8_t *>(&v);
    payload.insert(payload.end(), b, b + 4);  // float32 little-endian, see codec
  }
  // Capture any downlink that lands in RX1/RX2. lenDown is in/out: capacity in,
  // actual out. Blocks through the RX windows — the timing risk this spike exists
  // to measure.
  uint8_t down[RADIOLIB_LORAWAN_MAX_DOWNLINK_SIZE];
  size_t down_len = sizeof(down);
  LoRaWANEvent_t down_event{};
  int16_t state;
  {
    WdtPause wdt_pause;
    state = this->node_->sendReceive(payload.data(), payload.size(), 1, down, &down_len, false,
                                     nullptr, &down_event);
  }
  this->save_nonces_();
  if (state < RADIOLIB_ERR_NONE) {
    ESP_LOGW(TAG, "uplink failed: %d", state);
    return;
  }
  ESP_LOGD(TAG, "uplink sent (%u bytes)", (unsigned) payload.size());
  // state is the RX window (1 or 2) when a downlink arrived, 0 when none.
  if (state > 0 && down_len > 0) {
    ESP_LOGD(TAG, "downlink fport=%u (%u bytes)%s", down_event.fPort, (unsigned) down_len,
             down_event.frmPending ? ", more pending" : "");
    std::vector<uint8_t> down_payload(down, down + down_len);
    for (auto *t : this->downlink_triggers_)
      t->trigger(down_event.fPort, down_payload);
  }
}

void LoRaWANComponent::setup() {
  ESP_LOGCONFIG(TAG, "setting up LoRaWAN...");
  if (!this->init_radio_()) {
    this->mark_failed();
    return;
  }
  this->nonces_pref_ = global_preferences->make_preference<NoncesBlob>(fnv1_hash("lorawan_nonces"));
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
  if (millis() - this->last_uplink_ < this->uplink_interval_ms_)
    return;
  this->last_uplink_ = millis();
  this->uplink_();
}

void LoRaWANComponent::dump_config() {
  ESP_LOGCONFIG(TAG, "LoRaWAN:");
  ESP_LOGCONFIG(TAG, "  chip: %s", this->chip_.c_str());
  ESP_LOGCONFIG(TAG, "  region: %s  sub_band: %u", this->region_.c_str(), this->sub_band_);
  ESP_LOGCONFIG(TAG, "  pins: cs=%d rst=%d dio/irq=%d busy=%d sck=%d miso=%d mosi=%d",
                this->cs_pin_, this->rst_pin_, this->irq_pin_, this->busy_pin_,
                this->sck_pin_, this->miso_pin_, this->mosi_pin_);
  ESP_LOGCONFIG(TAG, "  uplink_interval: %u ms", this->uplink_interval_ms_);
  ESP_LOGCONFIG(TAG, "  payload fields: %u", (unsigned) this->fields_.size());
}

}  // namespace lorawan
}  // namespace esphome
