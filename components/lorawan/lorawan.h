#pragma once

#include "esphome/core/component.h"
#include "esphome/core/preferences.h"
#include "esphome/components/sensor/sensor.h"

#include <RadioLib.h>

#include <string>
#include <vector>

namespace esphome {
namespace lorawan {

// Spike scope: OTAA join + periodic uplink of float32 sensor values on US915
// sub-band 2, with DevNonce/session persistence in ESPHome's NVS-backed
// preferences so a power cycle re-joins without a server-side nonce flush.
//
// The one unresolved risk is RX-window timing: RadioLib's sendReceive() blocks
// through RX1 (+1s) and RX2 (+2s), and ESPHome's loop is cooperative. This spike
// blocks in loop() on purpose to keep it minimal; making that non-blocking (or
// second-core) is the thing the spike must prove out before this is real.
class LoRaWANComponent : public Component {
 public:
  void setup() override;
  void loop() override;
  void dump_config() override;
  // After WiFi/SPI are up; radio init and the (blocking) join belong here.
  float get_setup_priority() const override { return setup_priority::AFTER_WIFI; }

  void set_chip(const std::string &chip) { this->chip_ = chip; }
  void set_radio_pins(int cs, int irq, int rst, int busy) {
    this->cs_pin_ = cs;
    this->irq_pin_ = irq;
    this->rst_pin_ = rst;
    this->busy_pin_ = busy;
  }
  void set_region(const std::string &region) { this->region_ = region; }
  void set_sub_band(uint8_t sub_band) { this->sub_band_ = sub_band; }
  void set_uplink_interval(uint32_t ms) { this->uplink_interval_ms_ = ms; }
  void set_credentials(const std::string &join_eui, const std::string &dev_eui,
                       const std::string &app_key);

  void add_payload_field(sensor::Sensor *s) { this->fields_.push_back(s); }

 protected:
  bool init_radio_();
  bool restore_nonces_();
  void save_nonces_();
  bool join_();
  void uplink_();

  std::string chip_;
  std::string region_{"US915"};
  int cs_pin_{-1};
  int irq_pin_{-1};
  int rst_pin_{-1};
  int busy_pin_{-1};
  uint8_t sub_band_{2};
  uint32_t uplink_interval_ms_{300000};
  uint32_t last_uplink_{0};

  uint64_t join_eui_{0};
  uint64_t dev_eui_{0};
  uint8_t app_key_[16]{};

  std::vector<sensor::Sensor *> fields_;

  // RadioLib owns the module/node; allocated in init_radio_() once the chip is
  // known. PhysicalLayer is the common base of all supported radios.
  PhysicalLayer *radio_{nullptr};
  LoRaWANNode *node_{nullptr};
  bool joined_{false};

  ESPPreferenceObject nonces_pref_;
};

}  // namespace lorawan
}  // namespace esphome
