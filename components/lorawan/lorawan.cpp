#include "lorawan.h"
#include "esphome/core/log.h"
#include "esphome/core/hal.h"

namespace esphome {
namespace lorawan {

static const char *const TAG = "lorawan";

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
  Module *mod = new Module(this->cs_pin_, this->irq_pin_, this->rst_pin_, this->busy_pin_);
  if (this->chip_ == "sx1276") {
    this->radio_ = new SX1276(mod);
  } else if (this->chip_ == "sx1278") {
    this->radio_ = new SX1278(mod);
  } else if (this->chip_ == "sx1262") {
    this->radio_ = new SX1262(mod);
  } else {
    ESP_LOGE(TAG, "unknown radio chip '%s'", this->chip_.c_str());
    return false;
  }

  // US915 is the only band exercised in the spike; the schema accepts others so
  // this switch can grow without a config change.
  const LoRaWANBand_t *band = &US915;
  this->node_ = new LoRaWANNode(this->radio_, band, this->sub_band_);
  return true;
}

bool LoRaWANComponent::restore_nonces_() {
  this->nonces_pref_ = global_preferences->make_preference<NoncesBlob>(fnv1_hash("lorawan_nonces"));
  NoncesBlob blob{};
  if (!this->nonces_pref_.load(&blob))
    return false;
  return this->node_->setBufferNonces(blob.data) == RADIOLIB_ERR_NONE;
}

void LoRaWANComponent::save_nonces_() {
  NoncesBlob blob{};
  memcpy(blob.data, this->node_->getBufferNonces(), sizeof(blob.data));
  this->nonces_pref_.save(&blob);
}

bool LoRaWANComponent::join_() {
  this->node_->beginOTAA(this->join_eui_, this->dev_eui_, nullptr, this->app_key_);
  // Blocks through the join RX windows. See the class comment in lorawan.h.
  int16_t state = this->node_->activateOTAA();
  this->save_nonces_();  // persist the new DevNonce regardless of outcome
  if (state == RADIOLIB_LORAWAN_NEW_SESSION || state == RADIOLIB_ERR_NONE) {
    ESP_LOGI(TAG, "OTAA join OK");
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
  // Blocks through RX1/RX2 — the timing risk this spike exists to measure.
  int16_t state = this->node_->sendReceive(payload.data(), payload.size(), 1);
  this->save_nonces_();
  if (state < RADIOLIB_ERR_NONE) {
    ESP_LOGW(TAG, "uplink failed: %d", state);
  } else {
    ESP_LOGD(TAG, "uplink sent (%u bytes)", (unsigned) payload.size());
  }
}

void LoRaWANComponent::setup() {
  ESP_LOGCONFIG(TAG, "setting up LoRaWAN...");
  int16_t state = this->radio_ == nullptr && !this->init_radio_() ? RADIOLIB_ERR_UNKNOWN : this->radio_->begin();
  if (state != RADIOLIB_ERR_NONE) {
    ESP_LOGE(TAG, "radio begin failed: %d", state);
    this->mark_failed();
    return;
  }
  this->restore_nonces_();  // best-effort; a fresh device just has none
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
  ESP_LOGCONFIG(TAG, "  uplink_interval: %u ms", this->uplink_interval_ms_);
  ESP_LOGCONFIG(TAG, "  payload fields: %u", (unsigned) this->fields_.size());
}

}  // namespace lorawan
}  // namespace esphome
