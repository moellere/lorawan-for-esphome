// ChirpStack decodeUplink for lorawan-for-esphome sensor payloads.
//
// Reference artifact: paste into the ChirpStack Device Profile codec. This is
// NOT server code the component runs -- the device half only emits the bytes.
//
// FIELDS lists the `sensor:` bindings in the order they appear in the ESPHome
// config, each with the same `type` and `multiplier` as its binding. A bare
// name means float32. Keep it in lockstep with the firmware
// (components/lorawan/payload.h); tests/codec checks the two against each
// other.
var FIELDS = [
  "battery",
  // { name: "temperature", type: "int16", multiplier: 100 },
];

var TYPES = {
  float32: { size: 4 },
  int8: { size: 1, signed: true },
  uint8: { size: 1, signed: false },
  int16: { size: 2, signed: true },
  uint16: { size: 2, signed: false },
  int32: { size: 4, signed: true },
  uint32: { size: 4, signed: false },
};

function field(f) {
  if (typeof f === "string") return { name: f, type: "float32", multiplier: 1 };
  return { name: f.name, type: f.type || "float32", multiplier: f.multiplier || 1 };
}

function uintle(b, o, n) {
  var v = 0;
  for (var i = n - 1; i >= 0; i--) v = v * 256 + b[o + i];
  return v;
}

function f32le(b, o) {
  var bits = uintle(b, o, 4);
  var sign = bits >= 0x80000000 ? -1 : 1;
  var exp = Math.floor(bits / 0x800000) & 0xff;
  var frac = bits % 0x800000;
  if (exp === 0) return sign * frac * Math.pow(2, -149); // subnormal / zero
  if (exp === 0xff) return frac ? NaN : sign * Infinity;
  return sign * (1 + frac * Math.pow(2, -23)) * Math.pow(2, exp - 127);
}

// One value. Integer types reserve a "no reading" code (minimum if signed,
// maximum if unsigned), which decodes as null.
function decodeField(b, o, f) {
  var t = TYPES[f.type];
  if (f.type === "float32") return f32le(b, o);
  var raw = uintle(b, o, t.size);
  var span = Math.pow(2, 8 * t.size);
  if (t.signed) {
    if (raw >= span / 2) raw -= span;
    if (raw === -span / 2) return null;
  } else if (raw === span - 1) {
    return null;
  }
  return raw / f.multiplier;
}

function decodeUplink(input) {
  var bytes = input.bytes;
  var fields = FIELDS.map(field);
  var expected = 0;
  for (var i = 0; i < fields.length; i++) {
    if (!TYPES[fields[i].type])
      return { data: {}, warnings: [], errors: ["unknown type " + fields[i].type] };
    expected += TYPES[fields[i].type].size;
  }
  if (bytes.length !== expected) {
    return { data: {}, warnings: [],
      errors: ["expected " + expected + " bytes for " + fields.length +
               " fields, got " + bytes.length] };
  }
  var data = {};
  var o = 0;
  for (var j = 0; j < fields.length; j++) {
    data[fields[j].name] = decodeField(bytes, o, fields[j]);
    o += TYPES[fields[j].type].size;
  }
  return { data: data, warnings: [], errors: [] };
}
