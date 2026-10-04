#pragma once

#include <RadioLib.h>
#include <string>

namespace esphome {
namespace lorawan {

// Keep this list identical to REGIONS in __init__.py. Fail closed for typos.
inline const LoRaWANBand_t *configured_band(const std::string &region) {
  if (region == "EU868") return &EU868;
  if (region == "US915") return &US915;
  if (region == "AU915") return &AU915;
  if (region == "AS923") return &AS923;
  return nullptr;
}

}  // namespace lorawan
}  // namespace esphome
