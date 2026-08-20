import assert from "node:assert/strict";
import { readFile } from "node:fs/promises";
import test from "node:test";
import { advanceDynamicTrackObjects, createMsvcRandom, sampleDynamicTrackObjects } from "../src/dynamic-track.js";
import { parseModelsIni } from "../src/kn5-workspace.js";

test("matches the shipped MSVCR120 random generator", () => {
  const random = createMsvcRandom(1);
  assert.deepEqual(Array.from({ length: 8 }, () => random.nextInt()), [41, 18467, 6334, 26500, 19169, 15724, 11478, 29358]);
  const skipped = createMsvcRandom(1), stepped = createMsvcRandom(1);
  skipped.skip(1000);
  for (let index = 0; index < 1000; index++) stepped.nextInt();
  assert.equal(skipped.nextInt(), stepped.nextInt());
});

test("samples native probability, inclusive multiplicity, position, and velocity order", () => {
  const state = sampleDynamicTrackObjects([{ name: "plane.kn5", dynamic: {
    index: 0, probability: 100, multiplicity: [2, 2], posMode: "RANDOM",
    positionCenter: [100, 200, 300], positionRange: [10, 20, 30], velMode: "RANDOM",
    velocityBase: [1, 2, 3], velocityRange: [4, 5, 6]
  } }], 1);
  assert.equal(state.results[0].accepted, true);
  assert.equal(state.results[0].sampledMultiplicity, 2);
  assert.deepEqual(state.results[0].instances, [
    { fileIndex: 0, instanceIndex: 0, position: [101.70018768310547, 212.34962463378906, 281.5982666015625], velocity: [4.167699337005615, 0.5029146671295166, 2.7584762573242188] },
    { fileIndex: 0, instanceIndex: 1, position: [93.48216247558594, 209.86419677734375, 319.3703918457031], velocity: [1.1082797050476074, 4.105014324188232, 7.307321548461914] }
  ]);
});

test("leaves non-random modes at the native zero transform", () => {
  const state = sampleDynamicTrackObjects([{ dynamic: {
    probability: 101, multiplicity: [1, 1], posMode: "FIXED", positionCenter: [10, 20, 30], positionRange: [1, 2, 3],
    velMode: "FIXED", velocityBase: [4, 5, 6], velocityRange: [1, 1, 1]
  } }], 7);
  assert.deepEqual(state.results[0].instances[0].position, [0, 0, 0]);
  assert.deepEqual(state.results[0].instances[0].velocity, [0, 0, 0]);
});

test("advances each instance with the native float position update", () => {
  const state = sampleDynamicTrackObjects([{ dynamic: {
    probability: 101, multiplicity: [1, 1], posMode: "FIXED", velMode: "RANDOM",
    velocityBase: [2, 4, 6], velocityRange: [0, 0, 0]
  } }], 3, { serverTime: 2 });
  assert.deepEqual(state.results[0].instances[0].position, [4, 8, 12]);
  advanceDynamicTrackObjects(state, 0.25);
  assert.deepEqual(state.results[0].instances[0].position, [4.5, 9, 13.5]);
  assert.equal(state.elapsed, 0.25);
});

test("bounds unsafe multiplicity without changing later random samples", () => {
  const state = sampleDynamicTrackObjects([
    { name: "many.kn5", dynamic: { probability: 101, multiplicity: [300, 300], posMode: "RANDOM", positionCenter: [0, 0, 0], positionRange: [1, 1, 1], velMode: "RANDOM", velocityBase: [0, 0, 0], velocityRange: [1, 1, 1] } },
    { name: "later.kn5", dynamic: { probability: 101, multiplicity: [1, 1], posMode: "RANDOM", positionCenter: [0, 0, 0], positionRange: [1, 1, 1], velMode: "FIXED" } }
  ], 9, { maximumPreviewInstances: 4 });
  assert.equal(state.nativeInstances, 301);
  assert.equal(state.previewInstances, 4);
  assert.equal(state.results[1].sampledMultiplicity, 1);
  assert.equal(state.results[1].instances.length, 0);
  assert.match(state.warnings[0], /sampled 300 instances/);
});

test("does not allocate or throw for overflowing multiplicity input", () => {
  const state = sampleDynamicTrackObjects([{ name: "overflow.kn5", dynamic: {
    probability: 101, multiplicity: [1e308, 1e308], posMode: "RANDOM",
    positionCenter: [0, 0, 0], positionRange: [1, 1, 1], velMode: "RANDOM",
    velocityBase: [0, 0, 0], velocityRange: [1, 1, 1]
  } }], 1, { maximumPreviewInstances: 4 });
  assert.equal(state.nativeInstances, 0);
  assert.equal(state.previewInstances, 0);
  assert.match(state.warnings[0], /non-finite multiplicity/);
});

test("samples the installed Kunos Barcelona dynamic objects", async (context) => {
  let text;
  try { text = await readFile("/mnt/D/SteamLibrary/steamapps/common/assettocorsa/content/tracks/ks_barcelona/models_layout_gp.ini", "utf8"); }
  catch { context.skip("The Assetto Corsa Barcelona fixture is not installed"); return; }
  const manifest = parseModelsIni(text, "models_layout_gp.ini");
  const state = sampleDynamicTrackObjects(manifest.dynamicObjects.map((dynamic) => ({ name: dynamic.file, dynamic })), 1);
  assert.deepEqual(state.results.map((result) => [result.index, result.accepted, result.sampledMultiplicity]), [[0, true, 1], [1, false, 0]]);
  assert.deepEqual(state.results[0].instances[0].position, [-149.9813232421875, 166.1748046875, 786.6085205078125]);
  assert.deepEqual(state.results[0].instances[0].velocity, [1.5838496685028076, 0, -0.08050787448883057]);
});
