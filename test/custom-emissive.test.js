import assert from "node:assert/strict";
import test from "node:test";
import { CSP_BOUNCEBACK_EXPONENT, customEmissiveBounceLobe, customEmissiveBounceMultiplier } from "../src/custom-emissive.js";

test("matches the CSP gamma-space bounce-back material multiplier", () => {
  const rule = { mask: [0, 1, 0, 0], intensity: 4 };
  assert.deepEqual(customEmissiveBounceMultiplier([0.1, 0.8, 0.2, 0], [0.25, 0.5, 1], 0.25, rule), [1.6, 3.2, 6.4]);
  const negative = customEmissiveBounceMultiplier([0.1, 0.8, 0.2, 0], [0.25, 0.5, 1], 0.25, { ...rule, intensity: -4 });
  assert.ok(negative.every((value, index) => Math.abs(value - [1.2, 2.4, 4.8][index]) < 1e-12));
  assert.deepEqual(customEmissiveBounceMultiplier([1, 1, 1, 1], [0.25, 0.5, 1], 0, { mask: [1, 1, 1, 1], intensity: 2 }), [1, 2, 4]);
});

test("matches the CSP directional bounce-back lobes", () => {
  assert.equal(CSP_BOUNCEBACK_EXPONENT, 80);
  assert.equal(customEmissiveBounceLobe([0, 0, 1], [0, 0, 1]), 1);
  assert.equal(customEmissiveBounceLobe([0, 0, 1], [0, 0, -1]), 0);
  assert.equal(customEmissiveBounceLobe([0, 0, 1], [0, 0, -1], true), 1);
  assert.ok(Math.abs(customEmissiveBounceLobe([0, 0, 1], [0, Math.sqrt(1 - 0.99 ** 2), 0.99]) - 0.99 ** 80) < 1e-12);
});
