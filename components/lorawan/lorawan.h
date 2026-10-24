#pragma once

#include "esphome/core/defines.h"
#include "esphome/core/component.h"
#include "esphome/core/automation.h"
#include "esphome/core/preferences.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/components/spi/spi.h"
#ifdef USE_DEEP_SLEEP
#include "esphome/components/deep_sleep/deep_sleep_component.h"
#endif

#include <RadioLib.h>

#include "esphome_hal.h"
#include "payload.h"

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

#include <string>
#include <vector>

namespace esphome {
namespace lorawan {

// Fired for each application downlink received in an RX window, with the
// downlink's fPort and raw payload bytes. Class A: downlinks only arrive in the
// window after an uplink, so latency is up to one uplink_interval.
class DownlinkTrigger : public Trigger<uint8_t, std::vector<uint8_t>> {};

// RadioLib's nonce and session buffers are fixed size; persist exactly those
// blobs in NVS.
struct NoncesBlob {
  uint8_t data[RADIOLIB_LORAWAN_NONCES_BUF_SIZE];
};
struct SessionBlob {
  uint8_t data[RADIOLIB_LORAWAN_SESSION_BUF_SIZE];
};

// OTAA join + uplink of ESPHome sensor values, with DevNonce/session
// persistence in ESPHome's NVS-backed preferences.
//
// RadioLib's join and sendReceive() block through the RX windows (5-6 s on a
// join, 1-2 s on an uplink), so they run on a dedicated FreeRTOS task that owns
// the radio. loop() never touches RadioLib after setup(): it queues join and
// uplink requests and, on the main task, handles what comes back -- logging,
// NVS writes, and on_downlink. The radio is an ordinary ESPHome SPI device, but
// its bus must not be shared with another device (the schema enforces this),
// since the task drives it concurrently with the main loop.
class LoRaWANComponent : public Component, public RadioSpi {
 public:
  void setup() override;
  void loop() override;
  void dump_config() override;
  // After WiFi/SPI are up; radio init and the (blocking) join belong here.
  float get_setup_priority() const override { return setup_priority::AFTER_WIFI; }

  void set_chip(const std::string &chip) { this->chip_ = chip; }
  void set_rst_pin(GPIOPin *pin) { this->rst_pin_ = pin; }
  void set_irq_pin(InternalGPIOPin *pin) { this->irq_pin_ = pin; }
  void set_busy_pin(GPIOPin *pin) { this->busy_pin_ = pin; }
  void set_tcxo_voltage(float v) { this->tcxo_voltage_ = v; }
  void set_dio2_as_rf_switch(bool on) { this->dio2_as_rf_switch_ = on; }
  void add_setup_high_pin(GPIOPin *pin) { this->setup_high_pins_.push_back(pin); }
  void add_setup_low_pin(GPIOPin *pin) { this->setup_low_pins_.push_back(pin); }
  void set_rxen_pin(GPIOPin *pin) { this->rxen_pin_ = pin; }
  void set_txen_pin(GPIOPin *pin) { this->txen_pin_ = pin; }
  void set_region(const std::string &region) { this->region_ = region; }
  void set_sub_band(uint8_t sub_band) { this->sub_band_ = sub_band; }
  void set_uplink_interval(uint32_t ms) { this->uplink_interval_ms_ = ms; }
  void set_credentials(const std::string &join_eui, const std::string &dev_eui,
                       const std::string &app_key);

  void set_device_class(const std::string &cls) { this->device_class_ = cls; }
#ifdef USE_DEEP_SLEEP
  void set_deep_sleep(deep_sleep::DeepSleepComponent *ds) { this->deep_sleep_ = ds; }
#endif

  void add_payload_field(sensor::Sensor *s, FieldType type, float multiplier) {
    this->fields_.push_back({s, type, multiplier});
  }
  void add_on_downlink_trigger(DownlinkTrigger *t) { this->downlink_triggers_.push_back(t); }

  // Queue an arbitrary application payload for the radio task, on the given
  // fPort. This is the API for devices whose payload is not a sensor pack
  // (packed binary telemetry, protobufs, ...). It counts as *the* uplink for
  // interval purposes, and any downlink in its RX windows reaches on_downlink.
  // Returns false when not joined or the queue is full; true means queued, not
  // sent -- the outcome is logged when the radio task reports back.
  bool send_raw(uint8_t f_port, const std::vector<uint8_t> &payload);

  bool is_joined() const { return this->joined_; }

 protected:
  // What loop() hands the radio task.
  struct RadioRequest {
    enum Kind : uint8_t { JOIN, UPLINK } kind;
    uint8_t f_port;
    uint8_t len;
    uint8_t data[RADIOLIB_LORAWAN_MAX_PAYLOAD_SIZE];
  };
  // What the radio task hands back. Carries a copy of RadioLib's persistent
  // buffers taken right after the operation, so NVS is written on the main
  // task (ESPHome's preferences are not thread-safe).
  struct RadioResult {
    enum Kind : uint8_t { JOINED, JOIN_FAILED, UPLINK_SENT, UPLINK_FAILED, DOWNLINK, CLASS_C_FAILED } kind;
    int16_t state;
    int16_t class_state;
    uint8_t up_port;
    uint8_t up_len;
    uint8_t down_port;
    uint8_t down_len;
    bool frm_pending;
    bool has_state;
    bool has_session;
    uint8_t down[RADIOLIB_LORAWAN_MAX_PAYLOAD_SIZE];
    NoncesBlob nonces;
    SessionBlob session;
  };

  bool init_radio_();
  void restore_state_();
  void save_state_(const RadioResult &res);
  bool request_(RadioRequest::Kind kind, uint8_t f_port, const uint8_t *data, size_t len);
  void uplink_();
  void handle_result_(const RadioResult &res);
  void dispatch_downlink_(uint8_t f_port, const uint8_t *data, size_t len);

  // Radio task side: only these touch node_/radio_ once the task is running.
  static void radio_task_(void *arg);
  void radio_task_loop_();
  void radio_join_(RadioResult &res);
  void radio_uplink_(const RadioRequest &req, RadioResult &res);
  bool radio_poll_class_c_(RadioResult &res);
  void radio_snapshot_(RadioResult &res);

  std::string chip_;
  std::string region_{"US915"};
  // "A" (default) or "C". Class C keeps the receiver open between uplinks so
  // downlinks arrive in seconds instead of waiting for the next uplink's RX
  // window. RadioLib arms the continuous RxC window after the first uplink
  // following the join, so a Class C device should keep a nonzero
  // uplink_interval as its heartbeat.
  std::string device_class_{"A"};
  GPIOPin *rst_pin_{nullptr};
  InternalGPIOPin *irq_pin_{nullptr};
  GPIOPin *busy_pin_{nullptr};
  // Negative means "not configured": leave RadioLib on its own default
  // rather than forcing a crystal, since SX1262 boards are usually TCXO.
  float tcxo_voltage_{-1.0f};
  bool dio2_as_rf_switch_{false};
  // Front-end / PA enables asserted before the radio is touched.
  std::vector<GPIOPin *> setup_high_pins_;
  std::vector<GPIOPin *> setup_low_pins_;
  // Front-end selects RadioLib toggles per transfer: idle both LOW, txen HIGH
  // during TX, rxen HIGH during RX (e.g. the Heltec V4's GC1109 CPS, full PA vs
  // bypass, on GPIO46).
  GPIOPin *rxen_pin_{nullptr};
  GPIOPin *txen_pin_{nullptr};
  EsphomeHal *hal_{nullptr};
  uint8_t sub_band_{2};
  uint32_t uplink_interval_ms_{300000};
  uint32_t last_uplink_{0};

  uint64_t join_eui_{0};
  uint64_t dev_eui_{0};
  uint8_t app_key_[16]{};

  struct BoundField {
    sensor::Sensor *sensor;
    FieldType type;
    float multiplier;
  };
  std::vector<BoundField> fields_;
  std::vector<DownlinkTrigger *> downlink_triggers_;

  // RadioLib owns the module/node; allocated in init_radio_() once the chip is
  // known. PhysicalLayer is the common base of all supported radios.
  PhysicalLayer *radio_{nullptr};
  LoRaWANNode *node_{nullptr};
  bool joined_{false};

  ESPPreferenceObject nonces_pref_;
  ESPPreferenceObject session_pref_;

  QueueHandle_t requests_{nullptr};
  QueueHandle_t results_{nullptr};
  // Requests queued and not yet answered; loop() schedules nothing new while
  // one is on the radio.
  uint8_t in_flight_{0};
  // Main-task scratch for draining results_ (a RadioResult is ~600 bytes).
  RadioResult result_buf_{};
  // Radio-task only.
  bool class_c_active_{false};
  RadioResult task_result_{};

#ifdef USE_DEEP_SLEEP
  void deep_sleep_cycle_();
  deep_sleep::DeepSleepComponent *deep_sleep_{nullptr};
  uint32_t cycle_start_{0};
  bool uplink_queued_{false};
  bool cycle_done_{false};
#endif
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
