import assert from "node:assert/strict";
import { readFile } from "node:fs/promises";
import test from "node:test";
import { applyBottomColliderEdits, bottomColliderEditCount, captureBottomColliderBaseline, validateBottomColliderVector } from "../src/bottom-collider-authoring.js";

function config() {
  return { source: "data/colliders.ini", colliders: [{
    index: 0,
    section: "COLLIDER_0",
    line: 1,
    centre: [0, -0.2, 0.5],
    size: [1.8, 0.1, 3.8],
    groundEnabled: true,
    bounds: { min: [-0.9, -0.25, -1.4], max: [0.9, -0.15, 2.4] }
  }] };
}

test("applies and restores bottom-collider edits from a stable baseline", () => {
  const source = config(), baseline = captureBottomColliderBaseline(source), edits = { "0": { centre: [0.1, -0.3, 0.6], size: [1.9, 0.2, 4], groundEnabled: false } };
  assert.equal(bottomColliderEditCount(edits), 3);
  assert.equal(applyBottomColliderEdits(source, edits, baseline), 3);
  assert.deepEqual(source.colliders[0].centre, [0.1, -0.3, 0.6]);
  assert.deepEqual(source.colliders[0].size, [1.9, 0.2, 4]);
  assert.equal(source.colliders[0].groundEnabled, false);
  assert.deepEqual(source.colliders[0].bounds, { min: [-0.85, -0.4, -1.4], max: [1.05, -0.19999999999999998, 2.6] });
  assert.equal(applyBottomColliderEdits(source, {}, baseline), 0);
  assert.deepEqual(source, config());
});

test("ignores edits for invalid and unavailable collider positions", () => {
  const source = config(), baseline = captureBottomColliderBaseline(source);
  assert.equal(applyBottomColliderEdits(source, { bad: { centre: [1, 2, 3] }, "4": { size: [1, 1, 1] } }, baseline), 0);
  assert.deepEqual(source, config());
});

test("rejects bottom-collider sizes that underflow float32", () => {
  assert.throws(() => validateBottomColliderVector([1e-50, 1, 1], "size"), /positive float32/);
  assert.deepEqual(validateBottomColliderVector([1e-7, 1, 1], "size"), [1e-7, 1, 1]);
});

test("allows the collider overlay when only bottom boxes are available", async () => {
  const source = await readFile(new URL("../public/app.js", import.meta.url), "utf8");
  assert.match(source, /if\(!renderer\|\|\(!carColliderMatchesOpenModel\(\)&&!bottomColliderMatchesOpenModel\(\)\)\)return/);
});
