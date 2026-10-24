// Decodes each vectors.cpp case with codec/decodeUplink.js and checks the
// result. Usage: ./vectors | node check.js
const fs = require("fs");
const path = require("path");
const vm = require("vm");

const src = fs.readFileSync(path.join(__dirname, "../../codec/decodeUplink.js"), "utf8");
const lines = fs.readFileSync(0, "utf8").trim().split("\n");

function same(got, want) {
  if (want === "NaN") return Number.isNaN(got);
  if (want === null) return got === null;
  return typeof got === "number" && Math.abs(got - want) <= 1e-9 * Math.max(1, Math.abs(want));
}

let failed = 0;
for (const line of lines) {
  const c = JSON.parse(line);
  const ctx = {};
  vm.runInNewContext(src, ctx);
  ctx.FIELDS = c.fields;
  const out = ctx.decodeUplink({ bytes: c.bytes, fPort: 1 });
  const problems = [...out.errors];
  c.fields.forEach((f, i) => {
    if (!same(out.data[f.name], c.expected[i]))
      problems.push(`${f.name}: got ${out.data[f.name]}, want ${c.expected[i]}`);
  });
  if (problems.length) {
    failed++;
    console.log(`FAIL ${line}\n  ${problems.join("\n  ")}`);
  }
}

const ctx = {};
vm.runInNewContext(src, ctx);
ctx.FIELDS = [{ name: "t", type: "int16", multiplier: 100 }];
if (ctx.decodeUplink({ bytes: [1, 2, 3], fPort: 1 }).errors.length !== 1) {
  failed++;
  console.log("FAIL a wrong-length payload is not rejected");
}

console.log(`${lines.length + 1 - failed}/${lines.length + 1} codec cases pass`);
process.exit(failed ? 1 : 0);
