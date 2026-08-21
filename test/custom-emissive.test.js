import assert from "node:assert/strict";
import { readFile } from "node:fs/promises";
import test from "node:test";
import {
  CSP_BOUNCEBACK_EXPONENT,
  customEmissiveBounceCoverage,
  customEmissiveBounceDirectSource,
  customEmissiveBounceLobe,
  customEmissiveBounceMultiplier
} from "../src/custom-emissive.js";

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

test("applies the diffuse color exactly once to direct bounce", () => {
  const source = customEmissiveBounceDirectSource([1, 0.5, 0.25], 1, 0.5, [0.005, 0.01, 0.015], [0.1, 0.2, 0.3]);
  assert.deepEqual(source, [0.605, 0.46, 0.44]);
  const multiplier = customEmissiveBounceMultiplier([1, 0, 0, 0], [0.5, 0.25, 0.125], 1, { mask: [1, 0, 0, 0], intensity: 1 });
  assert.deepEqual(source.map((value, index) => value * multiplier[index]), [0.605, 0.23, 0.11]);
});

test("skips the local-light bounce lobe when custom bounce is inactive", async () => {
  const source = await readFile(new URL("../public/app.js", import.meta.url), "utf8");
  assert.match(source, /bool customBounceActive=hasCustomBounce&&abs\(customBounceIntensity\)>1e-6;/);
  assert.match(source, /if\(customBounceActive&&cspLightFalloff\[i\]\.w<\.5\)localBounceSource\+=radiance\*pow/);
  assert.doesNotMatch(source, /if\(cspLightFalloff\[i\]\.w<\.5\)localBounceSource\+=radiance\*pow/);
});

test("uses final procedural color-mask coverage for bounce channels", () => {
  assert.deepEqual(
    customEmissiveBounceCoverage([0, 0, 0, 0], [{ channel: 1, coverage: 0.75, opacity: 0.5 }]),
    [0, 0.375, 0, 0]
  );
  assert.deepEqual(
    customEmissiveBounceCoverage([0, 1, 0, 0], [{ channel: 1, coverage: 0, opacity: 1 }], [1]),
    [0, 0, 0, 0]
  );
  assert.deepEqual(
    customEmissiveBounceCoverage([0, 1, 0, 0], [{ channel: 1, coverage: 0.4, opacity: 1 }], [1]),
    [0, 0.4, 0, 0]
  );
});
