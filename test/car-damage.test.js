import assert from "node:assert/strict";
import { readFile } from "node:fs/promises";
import { join } from "node:path";
import test from "node:test";
import {
  applyCarDamageEdits, captureCarDamageBaseline, carDamageEditCount,
  MAXIMUM_CAR_DAMAGE_BYTES, parseCarDamageIni, readCarDamageFile, serializeCarDamageIni
} from "../src/car-damage.js";

const fixture = join(import.meta.dirname, "content", "cars", "619_gen6_arca_base", "data", "damage.ini");

test("parses the repository car damage file", async () => {
  const parsed = parseCarDamageIni(await readFile(fixture, "utf8"), fixture);
  assert.deepEqual(parsed.scratches, { section: "SCRATCHES", line: 1, minSpeed: 0, maxSpeed: 20, extraEntries: [] });
  assert.equal(parsed.oscillations.enabled, true);
  assert.equal(parsed.damage.initialLevel, 20);
  assert.equal(parsed.visualObjects.length, 2);
  assert.deepEqual(parsed.visualObjects[0].staticRotationAxis, [-1, 1, 1]);
  assert.equal(parsed.visualObjects[0].damageZone, "FRONT");
  assert.deepEqual(parsed.warnings, []);
});

test("rejects truncated and unsafe damage fields with controlled warnings", () => {
  const parsed = parseCarDamageIni(`[SCRATCHES]\nMAX_SPEED=-1\nMIN_SPEED=4\n[VISUAL_OBJECT_0]\nNAME=\nSTATIC_ROTATION_AXIS=1,2\n[VISUAL_OBJECT_2]\nNAME=HOOD\nSTATIC_ROTATION_AXIS=1e100,0,0\nDAMAGE_ZONE=front;bad\nFULL_SPEED=-2\nOSCILLATION_MIN_ANGLE=3\nOSCILLATION_MAX_ANGLE=2\n[VISUAL_OBJECT_999999999999999999999999]\nNAME=ATTACK\n`);
  assert.equal(parsed.visualObjects.length, 1);
  assert.deepEqual(parsed.visualObjects[0].staticRotationAxis, [0, 0, 0]);
  assert.equal(parsed.visualObjects[0].damageZone, "FRONT");
  assert.match(parsed.warnings.join("\n"), /nonnegative speeds/);
  assert.match(parsed.warnings.join("\n"), /NAME must contain/);
  assert.match(parsed.warnings.join("\n"), /finite float32/);
  assert.match(parsed.warnings.join("\n"), /OSCILLATION_MAX_ANGLE/);
  assert.match(parsed.warnings.join("\n"), /index must be from 0 to 1023/);
});

test("rejects blank scalar and vector components", () => {
  const parsed = parseCarDamageIni(`[SCRATCHES]\nMAX_SPEED=\nMIN_SPEED=0\n[VISUAL_OBJECT_0]\nNAME=HOOD\nSTATIC_ROTATION_AXIS=1,,2\n`);
  assert.equal(parsed.scratches.maxSpeed, 20);
  assert.deepEqual(parsed.visualObjects[0].staticRotationAxis, [0, 0, 0]);
  assert.match(parsed.warnings.join("\n"), /SCRATCHES MAX_SPEED must be a finite float32 number/);
  assert.match(parsed.warnings.join("\n"), /STATIC_ROTATION_AXIS must contain three finite float32 numbers/);
});

test("checks the damage file size before reading", async () => {
  let reads = 0;
  const file = { size: MAXIMUM_CAR_DAMAGE_BYTES + 1, async arrayBuffer() { reads++; return new ArrayBuffer(0); } };
  await assert.rejects(() => readCarDamageFile(file, "oversized/damage.ini"), /1 MiB input limit/);
  assert.equal(reads, 0);
});

test("round-trips known fields and retains safe unknown entries", () => {
  const source = parseCarDamageIni(`[HEADER]\nVERSION=3\n[SCRATCHES]\nMAX_SPEED=20\nMIN_SPEED=0\nCUSTOM=ok\n[OSCILLATIONS]\nENABLED=1\n[DAMAGE]\nINITIAL_LEVEL=20\n[VISUAL_OBJECT_0]\nNAME=HOOD\nSTATIC_ROTATION_AXIS=-1,1,1\nSTATIC_ROTATION_ANGLE=1\nMULT_G=0.01\nDAMAGE_ZONE=FRONT\nMIN_SPEED=20\nFULL_SPEED=80\nOSCILLATION_AXIS=1,1,1\nOSCILLATION_MIN_ANGLE=-2\nOSCILLATION_MAX_ANGLE=5\nALLOWED_G=1,1,1\nEXTRA_VALUE=retained\n`);
  const text = serializeCarDamageIni(source), parsed = parseCarDamageIni(text);
  assert.match(text, /\[HEADER\]\nVERSION=3/);
  assert.match(text, /CUSTOM=ok/);
  assert.match(text, /EXTRA_VALUE=retained/);
  assert.equal(parsed.visualObjects[0].name, "HOOD");
  assert.deepEqual(parsed.warnings, []);
});

test("preserves small floats and canonicalizes padded visual-object sections", () => {
  const source = parseCarDamageIni(`[VISUAL_OBJECT_00]\nNAME=HOOD\nMULT_G=0.0000001\n`);
  source.visualObjects[0].name = "HOOD_DAMAGE";
  const text = serializeCarDamageIni(source), parsed = parseCarDamageIni(text);
  assert.match(text, /\[VISUAL_OBJECT_0\]/);
  assert.doesNotMatch(text, /\[VISUAL_OBJECT_00\]/);
  assert.match(text, /MULT_G=1e-7/);
  assert.equal(parsed.visualObjects[0].name, "HOOD_DAMAGE");
  assert.equal(parsed.visualObjects[0].multG, 1e-7);
});

test("does not retain rejected visual-object sections as extras", () => {
  const source = parseCarDamageIni(`[HEADER]\nVERSION=3\n[VISUAL_OBJECT_2048]\nNAME=ATTACK\n`), text = serializeCarDamageIni(source);
  assert.deepEqual(source.extraSections.map((section) => section.name), ["HEADER"]);
  assert.doesNotMatch(text, /VISUAL_OBJECT_2048/);
  assert.match(text, /\[HEADER\]/);
});

test("rejects invalid authored damage output", () => {
  const parsed = parseCarDamageIni(`[SCRATCHES]\nMAX_SPEED=20\nMIN_SPEED=0\n[OSCILLATIONS]\nENABLED=1\n[DAMAGE]\nINITIAL_LEVEL=20\n[VISUAL_OBJECT_0]\nNAME=HOOD\n`);
  parsed.visualObjects[0].fullSpeed = -1;
  assert.throws(() => serializeCarDamageIni(parsed), /ordered speeds/);
  parsed.visualObjects[0].fullSpeed = 80;
  parsed.visualObjects[0].staticRotationAxis = [1e100, 0, 0];
  assert.throws(() => serializeCarDamageIni(parsed), /finite float32/);
  parsed.visualObjects[0].staticRotationAxis = [0, 0, 0];
  parsed.extraSections.push({ name: "BAD]", entries: [] });
  assert.throws(() => serializeCarDamageIni(parsed), /bracket/);
});

test("applies and restores damage edits from one stable baseline", () => {
  const parsed = parseCarDamageIni(`[SCRATCHES]\nMAX_SPEED=20\nMIN_SPEED=0\n[OSCILLATIONS]\nENABLED=1\n[DAMAGE]\nINITIAL_LEVEL=20\n[VISUAL_OBJECT_0]\nNAME=HOOD\nMIN_SPEED=20\nFULL_SPEED=80\n`);
  const baseline = captureCarDamageBaseline(parsed), edits = {
    SCRATCHES: { maxSpeed: 30 }, DAMAGE: { initialLevel: 40 },
    VISUAL_OBJECT_0: { name: "HOOD_DAMAGE", staticRotationAxis: [1, 0, 0] }
  };
  assert.equal(carDamageEditCount(edits), 4);
  assert.equal(applyCarDamageEdits(parsed, edits, baseline), 4);
  assert.equal(parsed.scratches.maxSpeed, 30);
  assert.equal(parsed.damage.initialLevel, 40);
  assert.equal(parsed.visualObjects[0].name, "HOOD_DAMAGE");
  assert.deepEqual(parsed.visualObjects[0].staticRotationAxis, [1, 0, 0]);
  assert.equal(applyCarDamageEdits(parsed, {}, baseline), 0);
  assert.equal(parsed.scratches.maxSpeed, 20);
  assert.equal(parsed.visualObjects[0].name, "HOOD");
});
