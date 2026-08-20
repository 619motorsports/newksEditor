import assert from "node:assert/strict";
import test from "node:test";
import { readFile } from "node:fs/promises";
import { analogRpmAngle, analogRpmRotation, analogRpmTransform, bindAnalogRpm, parseAnalogInstrumentsIni } from "../src/analog-instruments.js";
import { parseKn5 } from "../src/kn5.js";
import { assettoPath } from "./fixture-paths.js";

test("parses a linear analog RPM instrument", () => {
  const parsed = parseAnalogInstrumentsIni("[RPM_INDICATOR]\nOBJECT_NAME=ARROW_RPM\nZERO=3\nMIN_VALUE=1000\nSTEP=0.02766667\n");
  assert.deepEqual(parsed.rpm, { source: "data/analog_instruments.ini", line: 1, objectName: "ARROW_RPM", zero: 3, minValue: 1000, step: 0.02766667, lut: "", previewSupported: true });
  assert.deepEqual(parsed.warnings, []);
});

test("uses the recovered ksEditor local positive-Z RPM rotation", () => {
  const config = { zero: 3, minValue: 1000, step: 0.02766667 };
  assert.ok(Math.abs(analogRpmAngle(config, 1000) - 3 * Math.PI / 180) < 1e-12);
  assert.ok(Math.abs(analogRpmAngle(config, 6000) - 141.33335 * Math.PI / 180) < 1e-12);
  const rotation = analogRpmRotation(config, 1000);
  assert.ok(Math.abs(rotation[0] - Math.cos(3 * Math.PI / 180)) < 1e-12);
  assert.ok(Math.abs(rotation[1] - Math.sin(3 * Math.PI / 180)) < 1e-12);
  assert.deepEqual(rotation.slice(8), [0, 0, 1, 0, 0, 0, 0, 1]);
  const translated = analogRpmTransform([1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 12, 13, 14, 1], config, 1000);
  assert.deepEqual(translated.slice(12), [12, 13, 14, 1]);
});

test("binds the first exact RPM node and diagnoses duplicate names", () => {
  const first = { name: "ARROW_RPM", transform: new Array(16).fill(0), children: [] }, second = { name: "ARROW_RPM", transform: new Array(16).fill(1), children: [] };
  const model = { root: { name: "ROOT", children: [{ name: "arrow_rpm", transform: new Array(16).fill(2), children: [] }, first, second] } };
  const binding = bindAnalogRpm(model, { objectName: "ARROW_RPM" });
  assert.equal(binding.node, first);
  assert.equal(binding.matches, 2);
  assert.equal(binding.status, "ambiguous");
  assert.throws(() => analogRpmAngle({ zero: 0, minValue: 0, step: Infinity }, 1000), /valid RPM config/);
});

test("rejects malformed RPM values, oversized names, LUT previews, and oversized input", () => {
  const malformed = parseAnalogInstrumentsIni(`[RPM_INDICATOR]\nOBJECT_NAME=${"A".repeat(1025)}\nZERO=0\nMIN_VALUE=0\nSTEP=Infinity\n`);
  assert.equal(malformed.rpm, null);
  assert.ok(malformed.warnings.some((warning) => warning.includes("OBJECT_NAME is too long")));
  assert.ok(malformed.warnings.some((warning) => warning.includes("STEP must be finite")));
  const lut = parseAnalogInstrumentsIni("[RPM_INDICATOR]\nOBJECT_NAME=ARROW_RPM\nZERO=0\nMIN_VALUE=0\nSTEP=.03\nLUT=(0=0|1000=30)\n");
  assert.equal(lut.rpm.previewSupported, false);
  assert.ok(lut.warnings.some((warning) => warning.includes("LUT preview is not supported")));
  assert.throws(() => parseAnalogInstrumentsIni(" ".repeat(4 * 1024 * 1024 + 1)), /too large/);
});

test("binds the installed Porsche RPM needle", async (t) => {
  const directory = assettoPath("content/cars/ks_porsche_917_30"), configPath = `${directory}/data/analog_instruments.ini`, modelPath = `${directory}/porsche_917_30.kn5`;
  let text, bytes;
  try { [text, bytes] = await Promise.all([readFile(configPath, "utf8"), readFile(modelPath)]); }
  catch { t.skip("Assetto Corsa Porsche fixtures are not installed"); return; }
  const parsed = parseAnalogInstrumentsIni(text, configPath), binding = bindAnalogRpm(parseKn5(bytes, modelPath), parsed.rpm);
  assert.equal(parsed.rpm.objectName, "ARROW_RPM");
  assert.equal(binding.status, "resolved");
  assert.equal(binding.matches, 1);
});
