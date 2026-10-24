#pragma once

// Sensor-field packing for the uplink payload. Pure C++ on purpose (no ESPHome
// or RadioLib): tests/codec compiles this on the host and checks its bytes
// against codec/decodeUplink.js, so the device and the server decoder cannot
// drift apart.

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace esphome {
namespace lorawan {

enum class FieldType : uint8_t { FLOAT32, INT8, UINT8, INT16, UINT16, INT32, UINT32 };

inline size_t field_size(FieldType type) {
  switch (type) {
    case FieldType::INT8:
    case FieldType::UINT8:
      return 1;
    case FieldType::INT16:
    case FieldType::UINT16:
      return 2;
    default:
      return 4;
  }
}

// Little-endian. float32 is the raw IEEE value (NaN stays NaN). The integer
// types send round(value * multiplier), saturated, and reserve one code for
// "no reading" (a NaN value): the minimum of a signed type, the maximum of an
// unsigned one. Real values never take that code; they clamp one step inside.
inline void pack_field(std::vector<uint8_t> &out, FieldType type, float multiplier, float value) {
  uint32_t bits;
  if (type == FieldType::FLOAT32) {
    memcpy(&bits, &value, sizeof(bits));
  } else {
    int64_t lo, hi, none;
    switch (type) {
      case FieldType::INT8:
        lo = INT8_MIN + 1, hi = INT8_MAX, none = INT8_MIN;
        break;
      case FieldType::UINT8:
        lo = 0, hi = UINT8_MAX - 1, none = UINT8_MAX;
        break;
      case FieldType::INT16:
        lo = INT16_MIN + 1, hi = INT16_MAX, none = INT16_MIN;
        break;
      case FieldType::UINT16:
        lo = 0, hi = UINT16_MAX - 1, none = UINT16_MAX;
        break;
      case FieldType::INT32:
        lo = INT32_MIN + 1LL, hi = INT32_MAX, none = INT32_MIN;
        break;
      default:  // UINT32
        lo = 0, hi = UINT32_MAX - 1LL, none = UINT32_MAX;
        break;
    }
    int64_t v = none;
    if (!std::isnan(value)) {
      double scaled = std::round((double) value * (double) multiplier);
      v = scaled < (double) lo ? lo : scaled > (double) hi ? hi : (int64_t) scaled;
    }
    bits = (uint32_t) v;  // two's complement low 32 bits; the byte loop takes what it needs
  }
  for (size_t i = 0; i < field_size(type); i++)
    out.push_back((uint8_t) (bits >> (8 * i)));
}

}  // namespace lorawan
}  // namespace esphome
