import assert from "node:assert/strict";
import { readFile } from "node:fs/promises";
import test from "node:test";
import { customEmissiveUv, customEmissiveUvInBounds, normalizeCustomEmissiveMirrorDirection } from "../src/custom-emissive-uv.js";

test("reproduces CSP MirrorUV reflection on the negative half-plane", () => {
  const mirror = { offset: 0.6, direction: [1, 0] };
  assert.deepEqual(customEmissiveUv([0.2, 0.25], mirror), [1, 0.25]);
  assert.deepEqual(customEmissiveUv([0.8, 0.25], mirror), [0.8, 0.25]);
});

test("takes fractional UVs before the normal MirrorUV path", () => {
  const mirror = { offset: 0.6, direction: [1, 0] };
  assert.deepEqual(customEmissiveUv([1.2, -0.75], mirror), [1, 0.25]);
  assert.deepEqual(customEmissiveUv([1.2, -0.75], mirror, true), [1.2, -0.75]);
});

test("supports arbitrary two-dimensional MirrorUV planes", () => {
  const mirror = { offset: 0.5, direction: [0, 2] };
  assert.deepEqual(customEmissiveUv([0.25, 0.2], mirror), [0.25, 0.8]);
});

test("rejects reflected coordinates that would wrap across the bounded atlas", async () => {
  assert.equal(customEmissiveUvInBounds(customEmissiveUv([0.1, 0.25], { offset: 0.6, direction: [1, 0] })), false);
  assert.equal(customEmissiveUvInBounds(customEmissiveUv([0.3, 0.25], { offset: 0.6, direction: [1, 0] })), true);
  const source = await readFile(new URL("../public/app.js", import.meta.url), "utf8");
  assert.match(source, /bool customUvInBounds=customMirrorUv\.x<0\.5\|\|all\(greaterThanEqual\(customUv,vec2\(0\.0\)\)\)&&all\(lessThan\(customUv,vec2\(1\.0\)\)\);/);
  assert.match(source, /hasCustomEmissive&&customUvInBounds\?texture\(customEmissiveTexture,customUv\)/);
});

test("bounds malformed MirrorUV directions without non-finite output", () => {
  assert.deepEqual(normalizeCustomEmissiveMirrorDirection([0, 0, 99]), [0, 0]);
  assert.deepEqual(normalizeCustomEmissiveMirrorDirection([Number.NaN, Number.POSITIVE_INFINITY]), [0, 0]);
  assert.deepEqual(customEmissiveUv([Number.NaN, Number.NEGATIVE_INFINITY], { offset: Number.NaN, direction: [0, 0] }), [0, 0]);
});
