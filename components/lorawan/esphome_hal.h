#pragma once

#include "esphome/core/gpio.h"
#include "esphome/components/spi/spi.h"

#include <RadioLib.h>

namespace esphome {
namespace lorawan {

// The SPI device type the component is. Mode 0, MSB first; the data rate here
// is only the default, the YAML's data_rate overrides it.
using RadioSpi = spi::SPIDevice<spi::BIT_ORDER_MSB_FIRST, spi::CLOCK_POLARITY_LOW, spi::CLOCK_PHASE_LEADING,
                                spi::DATA_RATE_8MHZ>;

// RadioLib addresses pins by number; these are the numbers this HAL hands to
// RadioLib's Module, each backed by an ESPHome pin.
enum HalPin : uint32_t { HAL_PIN_CS = 0, HAL_PIN_IRQ, HAL_PIN_RST, HAL_PIN_BUSY, HAL_PIN_RXEN, HAL_PIN_TXEN, HAL_PIN_COUNT };

// RadioLib HAL over ESPHome's SPI and GPIO, so the radio is an ordinary ESPHome
// SPI device and builds under ESP-IDF as well as Arduino. Chip select belongs
// to ESPHome: spiBeginTransaction()/spiEndTransaction() wrap SPIDevice's
// enable()/disable(), which drive CS, and RadioLib's own writes to HAL_PIN_CS
// are no-ops.
class EsphomeHal : public RadioLibHal {
 public:
  explicit EsphomeHal(RadioSpi *spi);

  void set_pin(HalPin pin, GPIOPin *gpio) { this->pins_[pin] = gpio; }
  void set_irq_pin(InternalGPIOPin *gpio) {
    this->irq_ = gpio;
    this->pins_[HAL_PIN_IRQ] = gpio;
  }

  void pinMode(uint32_t pin, uint32_t mode) override;
  void digitalWrite(uint32_t pin, uint32_t value) override;
  uint32_t digitalRead(uint32_t pin) override;
  void attachInterrupt(uint32_t interruptNum, void (*interruptCb)(void), uint32_t mode) override;
  void detachInterrupt(uint32_t interruptNum) override;
  void delay(RadioLibTime_t ms) override;
  void delayMicroseconds(RadioLibTime_t us) override;
  RadioLibTime_t millis() override;
  RadioLibTime_t micros() override;
  long pulseIn(uint32_t pin, uint32_t state, RadioLibTime_t timeout) override;
  void spiBegin() override {}
  void spiBeginTransaction() override;
  void spiTransfer(uint8_t *out, size_t len, uint8_t *in) override;
  void spiEndTransaction() override;
  void spiEnd() override {}
  void yield() override;

 protected:
  GPIOPin *pin_(uint32_t pin) const;

  RadioSpi *spi_;
  GPIOPin *pins_[HAL_PIN_COUNT]{};
  InternalGPIOPin *irq_{nullptr};
  // RadioLib's ISR takes no argument; ESPHome's passes one. Keep the callback
  // here and hand the ISR a pointer to it.
  void (*irq_cb_)(){nullptr};
};

}  // namespace lorawan
}  // namespace esphome
