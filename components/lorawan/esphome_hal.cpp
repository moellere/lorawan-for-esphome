#include "esphome_hal.h"
#include "esphome/core/hal.h"

#include <cstring>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

namespace esphome {
namespace lorawan {

// The HAL constants RadioLib passes back to pinMode()/digitalWrite()/
// attachInterrupt(); their values only have to be distinct.
static constexpr uint32_t MODE_INPUT = 0;
static constexpr uint32_t MODE_OUTPUT = 1;
static constexpr uint32_t LEVEL_LOW = 0;
static constexpr uint32_t LEVEL_HIGH = 1;
static constexpr uint32_t IRQ_RISING = 1;
static constexpr uint32_t IRQ_FALLING = 2;

EsphomeHal::EsphomeHal(RadioSpi *spi)
    : RadioLibHal(MODE_INPUT, MODE_OUTPUT, LEVEL_LOW, LEVEL_HIGH, IRQ_RISING, IRQ_FALLING), spi_(spi) {}

GPIOPin *EsphomeHal::pin_(uint32_t pin) const {
  // RADIOLIB_NC, an unwired optional pin, and HAL_PIN_CS (ESPHome owns CS) all
  // come back null: the caller does nothing.
  if (pin >= HAL_PIN_COUNT || pin == HAL_PIN_CS)
    return nullptr;
  return this->pins_[pin];
}

void EsphomeHal::pinMode(uint32_t pin, uint32_t mode) {
  GPIOPin *p = this->pin_(pin);
  if (p != nullptr)
    p->pin_mode(mode == MODE_OUTPUT ? gpio::FLAG_OUTPUT : gpio::FLAG_INPUT);
}

void EsphomeHal::digitalWrite(uint32_t pin, uint32_t value) {
  GPIOPin *p = this->pin_(pin);
  if (p != nullptr)
    p->digital_write(value == LEVEL_HIGH);
}

uint32_t EsphomeHal::digitalRead(uint32_t pin) {
  GPIOPin *p = this->pin_(pin);
  return (p != nullptr && p->digital_read()) ? LEVEL_HIGH : LEVEL_LOW;
}

static void IRAM_ATTR hal_isr(void (**cb)()) { (*cb)(); }

void EsphomeHal::attachInterrupt(uint32_t interruptNum, void (*interruptCb)(void), uint32_t mode) {
  // RadioLib only ever interrupts on the radio's DIO line (Module's irq pin).
  if (interruptNum != HAL_PIN_IRQ || this->irq_ == nullptr)
    return;
  this->irq_cb_ = interruptCb;
  this->irq_->attach_interrupt(hal_isr, &this->irq_cb_,
                               mode == IRQ_FALLING ? gpio::INTERRUPT_FALLING_EDGE : gpio::INTERRUPT_RISING_EDGE);
}

void EsphomeHal::detachInterrupt(uint32_t interruptNum) {
  if (interruptNum == HAL_PIN_IRQ && this->irq_ != nullptr)
    this->irq_->detach_interrupt();
}

void EsphomeHal::delay(RadioLibTime_t ms) { esphome::delay(ms); }

void EsphomeHal::delayMicroseconds(RadioLibTime_t us) { esphome::delayMicroseconds(us); }

RadioLibTime_t EsphomeHal::millis() { return esphome::millis(); }

RadioLibTime_t EsphomeHal::micros() { return esphome::micros(); }

// Only used by RadioLib's non-LoRa protocols.
long EsphomeHal::pulseIn(uint32_t pin, uint32_t state, RadioLibTime_t timeout) { return 0; }

void EsphomeHal::spiBeginTransaction() { this->spi_->enable(); }

void EsphomeHal::spiTransfer(uint8_t *out, size_t len, uint8_t *in) {
  // ESPHome transfers in place; RadioLib hands separate out/in buffers.
  if (in == nullptr) {
    this->spi_->write_array(out, len);
    return;
  }
  if (in != out)
    memcpy(in, out, len);
  this->spi_->transfer_array(in, len);
}

void EsphomeHal::spiEndTransaction() { this->spi_->disable(); }

void EsphomeHal::yield() { taskYIELD(); }

}  // namespace lorawan
}  // namespace esphome
