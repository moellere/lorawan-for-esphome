// Packs a set of values with the firmware's payload.h and prints each case as
// one JSON line: the field list, the packed bytes, and what the decoder must
// return. check.js feeds the bytes to codec/decodeUplink.js and compares.

#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <limits>
#include <string>
#include <vector>

#include "payload.h"

using esphome::lorawan::FieldType;
using esphome::lorawan::pack_field;

struct Field {
  const char *name;
  FieldType type;
  const char *type_name;
  float multiplier;
};

struct Case {
  std::vector<Field> fields;
  std::vector<float> values;
  std::vector<const char *> expected;  // decoded values, as JSON literals
};

static const float NaN = std::numeric_limits<float>::quiet_NaN();

int main() {
  const Field f32{"f", FieldType::FLOAT32, "float32", 1};
  const Field i8{"a", FieldType::INT8, "int8", 1};
  const Field u8{"b", FieldType::UINT8, "uint8", 2};
  const Field i16{"t", FieldType::INT16, "int16", 100};
  const Field u16{"h", FieldType::UINT16, "uint16", 10};
  const Field i32{"p", FieldType::INT32, "int32", 1000};
  const Field u32{"c", FieldType::UINT32, "uint32", 1};

  const std::vector<Case> cases = {
      {{f32}, {3.5f}, {"3.5"}},
      {{f32}, {-0.15625f}, {"-0.15625"}},
      {{f32}, {NaN}, {"\"NaN\""}},
      {{i16}, {21.37f}, {"21.37"}},
      {{i16}, {-40.0f}, {"-40"}},
      {{i16}, {NaN}, {"null"}},
      {{i16}, {1e9f}, {"327.67"}},
      {{i16}, {-1e9f}, {"-327.67"}},
      {{i8, u8}, {-5.0f, 3.24f}, {"-5", "3"}},
      {{i8, u8}, {-200.0f, -1.0f}, {"-127", "0"}},
      {{u8}, {1000.0f}, {"127"}},
      {{u16}, {65.43f}, {"65.4"}},
      {{u16}, {NaN}, {"null"}},
      {{i32, u32}, {-1234.5678f, 4000000000.0f}, {"-1234.568", "4000000000"}},
      {{i32, u32}, {NaN, NaN}, {"null", "null"}},
      {{f32, i16, u8}, {12.25f, 19.5f, 50.0f}, {"12.25", "19.5", "50"}},
  };

  for (const auto &c : cases) {
    std::vector<uint8_t> bytes;
    for (size_t i = 0; i < c.fields.size(); i++)
      pack_field(bytes, c.fields[i].type, c.fields[i].multiplier, c.values[i]);
    printf("{\"fields\":[");
    for (size_t i = 0; i < c.fields.size(); i++)
      printf("%s{\"name\":\"%s%zu\",\"type\":\"%s\",\"multiplier\":%g}", i ? "," : "", c.fields[i].name, i,
             c.fields[i].type_name, c.fields[i].multiplier);
    printf("],\"bytes\":[");
    for (size_t i = 0; i < bytes.size(); i++)
      printf("%s%u", i ? "," : "", (unsigned) bytes[i]);
    printf("],\"expected\":[");
    for (size_t i = 0; i < c.expected.size(); i++)
      printf("%s%s", i ? "," : "", c.expected[i]);
    printf("]}\n");
  }
  return 0;
}
