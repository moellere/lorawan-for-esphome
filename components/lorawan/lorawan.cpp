#include "lorawan.h"
#include "esphome/core/application.h"
#include "esphome/core/log.h"
#include "esphome/core/hal.h"

#include <SPI.h>

namespace esphome {
namespace lorawan {

static const char *const TAG = "lorawan";

namespace {
// The radio task runs on the loop task's core, above it in priority, so
// RadioLib's window timing wins whenever the two compete. RadioLib sleeps
// (vTaskDelay) through the long RX delays, which is when loop() runs; what it
// busy-waits on with yield() is bounded by one frame's airtime. The task is not
// on the Task WDT, and loop() no longer blocks, so nothing needs feeding.
constexpr uint32_t RADIO_TASK_STACK = 8192;
constexpr UBaseType_t RADIO_TASK_PRIORITY = 3;  // loopTask runs at 1
// Class C latches frames from the radio IRQ; polling the latch is cheap.
constexpr uint32_t CLASS_C_POLL_MS = 50;

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
  // begin() lives on the concrete radio, not PhysicalLayer, and the config's
  // modulation values are placeholders -- LoRaWANNode reprograms the channel
  // per uplink, so RadioLib's defaults are left as they are.
  ConfigLoRa_t cfg;
  int16_t state;
  if (this->chip_ == "sx1276") {
    auto *radio = new SX1276(mod);
    state = radio->begin(cfg);
    this->radio_ = radio;
  } else if (this->chip_ == "sx1278") {
    auto *radio = new SX1278(mod);
    state = radio->begin(cfg);
    this->radio_ = radio;
  } else if (this->chip_ == "sx1262") {
    auto *radio = new SX1262(mod);
    // The TCXO voltage has to be set before begin(): the SX1262 powers its
    // oscillator from DIO3, and if that is wrong the chip never clocks and
    // begin() fails ERR_SPI_CMD_TIMEOUT, which reads like miswired SPI.
    // RadioLib's own default is 1.6 V, so only override when configured.
    if (this->tcxo_voltage_ >= 0.0f) {
      radio->tcxoVoltage = this->tcxo_voltage_;
      ESP_LOGD(TAG, "sx1262 begin with tcxo %.2fV", this->tcxo_voltage_);
    }
    state = radio->begin(cfg);
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

  // US915 is the only band wired; the schema rejects every other region until
  // its band (and its datarate table, see min_datarate_for) is added here.
  const LoRaWANBand_t *band = &US915;
  this->node_ = new LoRaWANNode(this->radio_, band, this->sub_band_);
  return true;
}

void LoRaWANComponent::restore_state_() {
  NoncesBlob nonces{};
  if (!this->nonces_pref_.load(&nonces))
    return;
  // setBufferNonces validates the blob's checksum against keyCheckSum, which is
  // only set by beginOTAA -- so this must run after beginOTAA, not before.
  if (this->node_->setBufferNonces(nonces.data) != RADIOLIB_ERR_NONE)
    return;
  // The session must follow the nonces: RadioLib checks its signature against
  // the restored nonce buffer and discards a mismatched one, so a session from
  // other keys or an older join never resumes. A restored session makes
  // activateOTAA() return SESSION_RESTORED without any join airtime.
  SessionBlob session{};
  if (!this->session_pref_.load(&session))
    return;
  int16_t state = this->node_->setBufferSession(session.data);
  if (state != RADIOLIB_ERR_NONE)
    ESP_LOGD(TAG, "stored session not usable (%d), joining fresh", state);
}

void LoRaWANComponent::save_state_(const RadioResult &res) {
  if (!res.has_state)
    return;
  this->nonces_pref_.save(&res.nonces);
  if (res.has_session)
    this->session_pref_.save(&res.session);
  // ESPHome only queues save() and writes NVS on the flash_write_interval
  // (60 s default). A power cut inside that window would bring back an old
  // DevNonce (join silently rejected) or an old FCnt (uplinks dropped as
  // replays), so write through now.
  global_preferences->sync();
}

// ---- radio task: the only code that touches node_/radio_ once it runs ----

void LoRaWANComponent::radio_task_(void *arg) { static_cast<LoRaWANComponent *>(arg)->radio_task_loop_(); }

void LoRaWANComponent::radio_task_loop_() {
  RadioRequest req;
  for (;;) {
    TickType_t wait = this->class_c_active_ ? pdMS_TO_TICKS(CLASS_C_POLL_MS) : portMAX_DELAY;
    if (xQueueReceive(this->requests_, &req, wait) == pdTRUE) {
      this->task_result_ = RadioResult{};
      if (req.kind == RadioRequest::JOIN) {
        this->radio_join_(this->task_result_);
      } else {
        this->radio_uplink_(req, this->task_result_);
      }
      this->radio_snapshot_(this->task_result_);
      xQueueSend(this->results_, &this->task_result_, portMAX_DELAY);
    }
    if (this->class_c_active_) {
      this->task_result_ = RadioResult{};
      if (this->radio_poll_class_c_(this->task_result_))
        xQueueSend(this->results_, &this->task_result_, portMAX_DELAY);
    }
  }
}

void LoRaWANComponent::radio_join_(RadioResult &res) {
  // beginOTAA() and the NVS restore already ran in setup(). Calling
  // activateOTAA() again after a failure is RadioLib's documented retry: the
  // DevNonce it advanced in memory carries into the next attempt.
  res.state = this->node_->activateOTAA();
  if (res.state != RADIOLIB_LORAWAN_NEW_SESSION && res.state != RADIOLIB_LORAWAN_SESSION_RESTORED) {
    res.kind = RadioResult::JOIN_FAILED;
    return;
  }
  res.kind = RadioResult::JOINED;
  if (this->device_class_ == "C") {
    // LoRaWAN 1.0.x switches immediately; 1.1 queues DeviceModeInd on the
    // next uplink. Either way RxC opens after the first post-join uplink.
    res.class_state = this->node_->setClass(RADIOLIB_LORAWAN_CLASS_C);
    this->class_c_active_ = res.class_state == RADIOLIB_ERR_NONE;
  }
}

void LoRaWANComponent::radio_uplink_(const RadioRequest &req, RadioResult &res) {
  // Re-assert the data rate this payload needs, every time. ADR drives the
  // rate down on a strong link, and US915 DR0 caps the application payload at
  // 11 bytes -- so a 12-byte payload (three float32s) joins fine, uplinks once
  // or twice, then fails with RADIOLIB_ERR_PACKET_TOO_LONG (-4) forever once
  // the network has settled the rate. Setting it once after join is not enough
  // because ADR lowers it again afterwards.
  this->node_->setDatarate(min_datarate_for(req.len));
  res.up_port = req.f_port;
  res.up_len = req.len;
  // RadioLib ignores the incoming lenDown (it zeroes it) and writes up to
  // MAX_PAYLOAD_SIZE bytes, so the buffer must be at least that.
  size_t down_len = 0;
  LoRaWANEvent_t down_event{};
  res.state = this->node_->sendReceive(req.data, req.len, req.f_port, res.down, &down_len, false,
                                       nullptr, &down_event);
  if (res.state < RADIOLIB_ERR_NONE) {
    res.kind = RadioResult::UPLINK_FAILED;
    return;
  }
  res.kind = RadioResult::UPLINK_SENT;
  // state is the RX window (1 or 2) when a downlink arrived, 0 when none.
  if (res.state > 0 && down_len > 0) {
    res.down_port = down_event.fPort;
    res.down_len = down_len;
    res.frm_pending = down_event.frmPending;
  }
}

// Class C: RadioLib re-arms continuous RX after every uplink and latches
// arriving frames from the radio IRQ; this just parses whatever latched.
bool LoRaWANComponent::radio_poll_class_c_(RadioResult &res) {
  size_t down_len = 0;
  LoRaWANEvent_t down_event{};
  res.state = this->node_->getDownlinkClassC(res.down, &down_len, &down_event);
  if (res.state > 0 && down_len > 0) {
    res.kind = RadioResult::DOWNLINK;
    res.down_port = down_event.fPort;
    res.down_len = down_len;
    return true;
  }
  if (res.state < RADIOLIB_ERR_NONE) {
    res.kind = RadioResult::CLASS_C_FAILED;
    return true;
  }
  return false;
}

void LoRaWANComponent::radio_snapshot_(RadioResult &res) {
  memcpy(res.nonces.data, this->node_->getBufferNonces(), sizeof(res.nonces.data));
  res.has_session = this->node_->isActivated();
  if (res.has_session)
    memcpy(res.session.data, this->node_->getBufferSession(), sizeof(res.session.data));
  res.has_state = true;
}

// ---- main task ----

bool LoRaWANComponent::request_(RadioRequest::Kind kind, uint8_t f_port, const uint8_t *data, size_t len) {
  if (this->requests_ == nullptr)
    return false;
  RadioRequest req{};
  if (len > sizeof(req.data)) {
    ESP_LOGW(TAG, "payload of %u bytes exceeds the %u-byte maximum", (unsigned) len, (unsigned) sizeof(req.data));
    return false;
  }
  req.kind = kind;
  req.f_port = f_port;
  req.len = len;
  if (len > 0)
    memcpy(req.data, data, len);
  if (xQueueSend(this->requests_, &req, 0) != pdTRUE) {
    ESP_LOGW(TAG, "radio busy, request dropped");
    return false;
  }
  this->in_flight_++;
  return true;
}

void LoRaWANComponent::handle_result_(const RadioResult &res) {
  switch (res.kind) {
    case RadioResult::JOINED:
      this->in_flight_--;
      this->save_state_(res);
      this->joined_ = true;
      this->status_clear_warning();
      ESP_LOGI(TAG, "OTAA join OK (%s)",
               res.state == RADIOLIB_LORAWAN_SESSION_RESTORED ? "restored" : "new session");
      if (this->device_class_ == "C") {
        if (res.class_state == RADIOLIB_ERR_NONE)
          ESP_LOGI(TAG, "device class C enabled");
        else
          ESP_LOGW(TAG, "setClass(C) failed: %d", res.class_state);
      }
      break;
    case RadioResult::JOIN_FAILED:
      this->in_flight_--;
      this->save_state_(res);  // persist the advanced DevNonce regardless of outcome
      ESP_LOGW(TAG, "OTAA join failed: %d", res.state);
      break;
    case RadioResult::UPLINK_SENT:
      this->in_flight_--;
      this->save_state_(res);
      // The DevEUI rides along because it is the only thing that identifies
      // this board to the network server, and nothing else emits it after
      // boot. A fleet tool reading a window of recent serial can then ask "who
      // is in this slot" without power-cycling the board to catch dump_config.
      ESP_LOGD(TAG, "uplink sent (%u bytes, fport=%u) dev_eui=%016llx", (unsigned) res.up_len,
               (unsigned) res.up_port, (unsigned long long) this->dev_eui_);
      if (res.down_len > 0) {
        ESP_LOGD(TAG, "downlink fport=%u (%u bytes)%s", (unsigned) res.down_port, (unsigned) res.down_len,
                 res.frm_pending ? ", more pending" : "");
        this->dispatch_downlink_(res.down_port, res.down, res.down_len);
      }
      break;
    case RadioResult::UPLINK_FAILED:
      this->in_flight_--;
      this->save_state_(res);
      ESP_LOGW(TAG, "uplink failed: %d", res.state);
      break;
    case RadioResult::DOWNLINK:
      ESP_LOGD(TAG, "class C downlink fport=%u (%u bytes)", (unsigned) res.down_port, (unsigned) res.down_len);
      this->dispatch_downlink_(res.down_port, res.down, res.down_len);
      break;
    case RadioResult::CLASS_C_FAILED:
      ESP_LOGW(TAG, "class C receive failed: %d", res.state);
      break;
  }
}

void LoRaWANComponent::dispatch_downlink_(uint8_t f_port, const uint8_t *data, size_t len) {
  std::vector<uint8_t> down_payload(data, data + len);
  for (auto *t : this->downlink_triggers_)
    t->trigger(f_port, down_payload);
}

void LoRaWANComponent::uplink_() {
  std::vector<uint8_t> payload;
  payload.reserve(this->fields_.size() * 4);
  for (auto *s : this->fields_) {
    float v = s->state;
    uint8_t *b = reinterpret_cast<uint8_t *>(&v);
    payload.insert(payload.end(), b, b + 4);  // float32 little-endian, see codec
  }
  this->request_(RadioRequest::UPLINK, 1, payload.data(), payload.size());
}

bool LoRaWANComponent::send_raw(uint8_t f_port, const std::vector<uint8_t> &payload) {
  if (!this->joined_) {
    ESP_LOGW(TAG, "send_raw dropped: not joined");
    return false;
  }
  // An explicit send counts as the uplink for interval purposes, so the
  // periodic sensor pack (if any) does not pile on right behind it.
  this->last_uplink_ = millis();
  return this->request_(RadioRequest::UPLINK, f_port, payload.data(), payload.size());
}

void LoRaWANComponent::setup() {
  ESP_LOGCONFIG(TAG, "setting up LoRaWAN...");
  if (!this->init_radio_()) {
#ifdef USE_DEEP_SLEEP
    // A failed component's loop() never runs, so in deep-sleep mode the device
    // would stay awake and drain the battery. Keep loop() alive to sleep.
    if (this->deep_sleep_ != nullptr) {
      this->status_set_error();
      return;
    }
#endif
    this->mark_failed();
    return;
  }
  // Key carries RadioLib's nonce-buffer version (NONCES_VERSION_VAL). 7.7 dropped
  // a byte from the layout, so a blob saved under an older RadioLib must never
  // reach setBufferNonces; a new key makes the old one unreachable.
  this->nonces_pref_ = global_preferences->make_preference<NoncesBlob>(fnv1_hash("lorawan_nonces_v3"));
  this->session_pref_ = global_preferences->make_preference<SessionBlob>(fnv1_hash("lorawan_session"));
  this->node_->beginOTAA(this->join_eui_, this->dev_eui_, nullptr, this->app_key_);
  // beginOTAA calls clearNonces(); restore the persisted DevNonce and session
  // afterwards so the DevNonce stays monotonic across reboots (or the server
  // drops the join silently) and a stored session resumes without a join.
  // Done here, before the radio task exists, so NVS stays on the main task.
  this->restore_state_();

  this->requests_ = xQueueCreate(2, sizeof(RadioRequest));
  this->results_ = xQueueCreate(4, sizeof(RadioResult));
  if (this->requests_ == nullptr || this->results_ == nullptr ||
      xTaskCreatePinnedToCore(LoRaWANComponent::radio_task_, "lorawan", RADIO_TASK_STACK, this,
                              RADIO_TASK_PRIORITY, nullptr, xPortGetCoreID()) != pdPASS) {
    ESP_LOGE(TAG, "could not start the radio task");
    this->requests_ = nullptr;
    this->results_ = nullptr;
#ifdef USE_DEEP_SLEEP
    if (this->deep_sleep_ != nullptr) {
      this->status_set_error();  // loop() still runs and sleeps; see above
      return;
    }
#endif
    this->mark_failed();
    return;
  }
  this->status_set_warning();  // cleared when the join lands
  this->last_uplink_ = millis();
  this->request_(RadioRequest::JOIN, 0, nullptr, 0);
}

void LoRaWANComponent::loop() {
  if (this->results_ != nullptr) {
    while (xQueueReceive(this->results_, &this->result_buf_, 0) == pdTRUE)
      this->handle_result_(this->result_buf_);
  }
#ifdef USE_DEEP_SLEEP
  if (this->deep_sleep_ != nullptr) {
    this->deep_sleep_cycle_();
    return;
  }
#endif
  // One request on the radio at a time from here; send_raw may add one.
  if (this->in_flight_ > 0)
    return;
  // Join retries and uplinks share the interval, which also respects duty cycle.
  if (millis() - this->last_uplink_ < this->uplink_interval_ms_)
    return;
  this->last_uplink_ = millis();
  if (!this->joined_) {
    this->request_(RadioRequest::JOIN, 0, nullptr, 0);
    return;
  }
  // No sensor fields configured means the payload is application-driven
  // (lorawan.send_raw); don't emit empty periodic uplinks on top of it.
  if (!this->fields_.empty())
    this->uplink_();
}

#ifdef USE_DEEP_SLEEP
// One uplink per wake. setup() queued the join (or the resume of the stored
// session); results are handled above, NVS included. What is left is to wait
// for the bound sensors, queue the one uplink, wait for it, and hand over to
// deep_sleep. A failed join just sleeps: the next wake retries, which spaces
// join attempts by sleep_duration.
void LoRaWANComponent::deep_sleep_cycle_() {
  if (this->cycle_done_ || this->in_flight_ > 0)
    return;
  if (this->joined_ && !this->uplink_queued_) {
    // ESPHome runs a sensor's first poll within min(update_interval / 2, 5 s)
    // of its setup, plus conversion time, so 10 s from the first loop() pass
    // after the join covers ordinary sensors; a slower one is uplinked as NaN.
    static const uint32_t SENSOR_WAIT_MS = 10000;
    if (this->cycle_start_ == 0)
      this->cycle_start_ = millis();
    bool ready = true;
    for (auto *s : this->fields_)
      ready = ready && s->has_state();
    if (!ready && millis() - this->cycle_start_ < SENSOR_WAIT_MS)
      return;
    if (!ready)
      ESP_LOGW(TAG, "uplinking before every bound sensor reported");
    this->uplink_queued_ = true;
    if (this->fields_.empty()) {
      ESP_LOGW(TAG, "deep sleep with no bound sensors: nothing to uplink this wake");
    } else {
      this->uplink_();
      if (this->in_flight_ > 0)
        return;  // sleep once the uplink comes back
    }
  } else if (!this->joined_) {
    ESP_LOGW(TAG, "not joined; sleeping, the next wake retries the join");
  }
  this->cycle_done_ = true;
  // RadioLib leaves the radio in standby, which idles at ~1.5 mA through the
  // whole sleep on an SX127x; the ESP's own deep sleep does not touch it. Safe
  // from the main task here: nothing is in flight and Class C is rejected with
  // deep sleep, so the radio task is parked on its queue. No node means begin()
  // failed, and the radio may not answer SPI at all.
  if (this->node_ != nullptr)
    this->radio_->sleep();
  // Not manual: deep_sleep.prevent (e.g. a maintenance window) still holds the
  // device awake, and deep_sleep retries from its own loop() once allowed.
  this->deep_sleep_->begin_sleep();
}
#endif

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
#ifdef USE_DEEP_SLEEP
  if (this->deep_sleep_ != nullptr)
    ESP_LOGCONFIG(TAG, "  deep sleep: one uplink per wake (deep_sleep sets the cadence)");
#endif
  ESP_LOGCONFIG(TAG, "  payload fields: %u", (unsigned) this->fields_.size());
}

}  // namespace lorawan
}  // namespace esphome
