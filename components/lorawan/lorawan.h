#pragma once

#include "esphome/core/component.h"
#include "esphome/core/automation.h"
#include "esphome/core/preferences.h"
#include "esphome/components/sensor/sensor.h"

#include <RadioLib.h>

#include <string>
#include <vector>

namespace esphome {
namespace lorawan {

// Fired for each application downlink received in an RX window, with the
// downlink's fPort and raw payload bytes. Class A: downlinks only arrive in the
// window after an uplink, so latency is up to one uplink_interval.
class DownlinkTrigger : public Trigger<uint8_t, std::vector<uint8_t>> {};

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
  void set_sck_pin(int8_t pin) { this->sck_pin_ = pin; }
  void set_miso_pin(int8_t pin) { this->miso_pin_ = pin; }
  void set_mosi_pin(int8_t pin) { this->mosi_pin_ = pin; }
  void set_tcxo_voltage(float v) { this->tcxo_voltage_ = v; }
  void set_dio2_as_rf_switch(bool on) { this->dio2_as_rf_switch_ = on; }
  void add_setup_high_pin(int pin) { this->setup_high_pins_.push_back(pin); }
  void add_setup_low_pin(int pin) { this->setup_low_pins_.push_back(pin); }
  void set_region(const std::string &region) { this->region_ = region; }
  void set_sub_band(uint8_t sub_band) { this->sub_band_ = sub_band; }
  void set_uplink_interval(uint32_t ms) { this->uplink_interval_ms_ = ms; }
  void set_credentials(const std::string &join_eui, const std::string &dev_eui,
                       const std::string &app_key);

  void set_device_class(const std::string &cls) { this->device_class_ = cls; }

  void add_payload_field(sensor::Sensor *s) { this->fields_.push_back(s); }
  void add_on_downlink_trigger(DownlinkTrigger *t) { this->downlink_triggers_.push_back(t); }

  // Send an arbitrary application payload right now, on the given fPort.
  // This is the API for devices whose payload is not a float32 sensor pack
  // (packed binary telemetry, protobufs, ...). Blocks through the Class A RX
  // windows exactly like the periodic uplink, counts as *the* uplink for
  // interval purposes, and feeds any downlink to on_downlink. Returns false
  // when not joined or the radio rejected the send.
  bool send_raw(uint8_t f_port, const std::vector<uint8_t> &payload);

  bool is_joined() const { return this->joined_; }

 protected:
  bool init_radio_();
  bool restore_nonces_();
  void save_nonces_();
  bool join_();
  void uplink_();
  bool transmit_(const uint8_t *data, size_t len, uint8_t f_port);
  void dispatch_downlink_(uint8_t f_port, const uint8_t *data, size_t len);
  void poll_class_c_();

  std::string chip_;
  std::string region_{"US915"};
  // "A" (default) or "C". Class C keeps the receiver open between uplinks so
  // downlinks arrive in seconds instead of waiting for the next uplink's RX
  // window. RadioLib arms the continuous RxC window after the first uplink
  // following the join, so a Class C device should keep a nonzero
  // uplink_interval as its heartbeat.
  std::string device_class_{"A"};
  int cs_pin_{-1};
  int irq_pin_{-1};
  int rst_pin_{-1};
  int busy_pin_{-1};
  // SPI bus pins; -1 leaves RadioLib on the framework's default SPI pins.
  int8_t sck_pin_{-1};
  int8_t miso_pin_{-1};
  int8_t mosi_pin_{-1};
  // Negative means "not configured": leave RadioLib on its own default
  // rather than forcing a crystal, since SX1262 boards are usually TCXO.
  float tcxo_voltage_{-1.0f};
  bool dio2_as_rf_switch_{false};
  // Front-end / PA enables asserted before the radio is touched.
  std::vector<int> setup_high_pins_;
  std::vector<int> setup_low_pins_;
  uint8_t sub_band_{2};
  uint32_t uplink_interval_ms_{300000};
  uint32_t last_uplink_{0};

  uint64_t join_eui_{0};
  uint64_t dev_eui_{0};
  uint8_t app_key_[16]{};

  std::vector<sensor::Sensor *> fields_;
  std::vector<DownlinkTrigger *> downlink_triggers_;

  // RadioLib owns the module/node; allocated in init_radio_() once the chip is
  // known. PhysicalLayer is the common base of all supported radios.
  PhysicalLayer *radio_{nullptr};
  LoRaWANNode *node_{nullptr};
  bool joined_{false};

  ESPPreferenceObject nonces_pref_;
};

// lorawan.send_raw action: fPort plus a templatable byte vector, so a lambda
// can pack binary telemetry on the fly.
template<typename... Ts> class SendRawAction : public Action<Ts...> {
 public:
  explicit SendRawAction(LoRaWANComponent *parent) : parent_(parent) {}
  TEMPLATABLE_VALUE(uint8_t, f_port)
  TEMPLATABLE_VALUE(std::vector<uint8_t>, payload)
  void play(Ts... x) override {
    this->parent_->send_raw(this->f_port_.value(x...), this->payload_.value(x...));
  }

 protected:
  LoRaWANComponent *parent_;
};

}  // namespace lorawan
}  // namespace esphome
