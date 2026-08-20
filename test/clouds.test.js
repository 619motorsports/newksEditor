import assert from "node:assert/strict";
import test from "node:test";
import { buildKsCloudBillboards, createKsCloudRandom, ksCloudShaderSample, KS_CLOUD_MAX_COUNT, KS_CLOUD_TEXTURE_PATHS, normalizeKsCloudSettings } from "../src/clouds.js";

test("reproduces the Visual C++ random sequence used by ksEditor clouds", () => {
  const random = createKsCloudRandom(1);
  assert.deepEqual(Array.from({ length: 5 }, () => Math.round(random() * 32767)), [41, 18467, 6334, 26500, 19169]);
});

test("builds the recovered stock cloud distribution deterministically", () => {
  const settings = { cloudWidth: 18, cloudHeight: 9, cloudRadius: 10, cloudNumber: 50, cloudBaseSpeed: 0.004 };
  const first = buildKsCloudBillboards(settings), second = buildKsCloudBillboards(settings);
  assert.equal(first.length, 50);
  assert.deepEqual(first, second);
  assert.equal(first[0].phi, 0);
  assert.deepEqual(first[0].position.slice(0, 2), [0, first[0].radius]);
  assert.equal(Math.abs(first[0].position[2]), 0);
  assert.ok(first.every((cloud) => cloud.texture >= 0 && cloud.texture < KS_CLOUD_TEXTURE_PATHS.length));
  assert.ok(first.every((cloud) => cloud.speed >= 0.0005 && cloud.speed <= settings.cloudBaseSpeed));
  assert.ok(first.every((cloud) => Math.abs(Math.hypot(...cloud.position) - cloud.radius) < 1e-12));
});

test("bounds malformed cloud settings before layout allocation", () => {
  const normalized = normalizeKsCloudSettings({ cloudWidth: -1, cloudHeight: Infinity, cloudRadius: "bad", cloudNumber: 1e12, cloudBaseSpeed: -4 });
  assert.deepEqual(normalized, { width: 0, height: 2, radius: 4, count: KS_CLOUD_MAX_COUNT, baseSpeed: 0 });
  assert.equal(buildKsCloudBillboards({ cloudNumber: 1e12 }).length, KS_CLOUD_MAX_COUNT);
  assert.deepEqual(buildKsCloudBillboards({ cloudNumber: 20 }, { worldDetail: -10 }), []);
  assert.deepEqual(buildKsCloudBillboards({ cloudNumber: 20 }, { textureCount: 0 }), []);
});

test("matches the recovered ksClouds pixel formula", () => {
  const sample = ksCloudShaderSample({ textureRed: 0.25, textureAlpha: 0.8, lightDirection: [0, -0.5, 0], lightColor: [2, 1, 0.5], ambientColor: [0.4, 0.2, 0.1], fogDistance: 12000, cloudCover: 0.5, cloudCutoff: 0.7, cloudColor: 3 });
  assert.equal(sample.fog, 0.075);
  assert.equal(sample.alpha, 0.4);
  assert.deepEqual(sample.rgb.map((value) => Number(value.toFixed(6))), [1.104188, 0.924188, 0.834187]);
  assert.equal(ksCloudShaderSample({ textureAlpha: 0.8, cloudCover: 4 }).alpha, 1);
});
