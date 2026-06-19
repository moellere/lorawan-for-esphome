// ChirpStack decodeUplink for lorawan-for-esphome float32-LE payloads.
//
// Reference artifact: paste into the ChirpStack Device Profile codec. This is
// NOT server code the component runs -- the device half only emits the bytes.
// Each bound sensor is one little-endian float32, in the same order the
// `sensor:` bindings appear in the ESPHome config. Keep FIELDS in lockstep with
// the firmware payload (lorawan.cpp uplink_()).
var FIELDS = ["battery"];

function f32le(b, o) {
  var bits = (b[o]) | (b[o + 1] << 8) | (b[o + 2] << 16) | (b[o + 3] << 24);
  var sign = (bits >>> 31) ? -1 : 1;
  var exp = (bits >>> 23) & 0xff;
  var frac = bits & 0x7fffff;
  if (exp === 0) return sign * frac * Math.pow(2, -149); // subnormal / zero
  if (exp === 0xff) return frac ? NaN : sign * Infinity;
  return sign * (1 + frac * Math.pow(2, -23)) * Math.pow(2, exp - 127);
}

function decodeUplink(input) {
  var bytes = input.bytes;
  var expected = FIELDS.length * 4;
  if (bytes.length !== expected) {
    return { data: {}, warnings: [],
      errors: ["expected " + expected + " bytes (" + FIELDS.length +
               " float32), got " + bytes.length] };
  }
  var data = {};
  for (var i = 0; i < FIELDS.length; i++) data[FIELDS[i]] = f32le(bytes, i * 4);
  return { data: data, warnings: [], errors: [] };
}
