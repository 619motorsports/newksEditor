import assert from "node:assert/strict";
import test from "node:test";
import { applySurfaceEdits, captureSurfaceBaseline, surfaceEditCount } from "../src/surface-authoring.js";

function config() {
  return { source: "data/surfaces.ini", surfaces: [
    { index: 0, key: "ROAD", friction: 0.98, damping: 0, dirtAdditive: 0, blackFlagTime: 0, isValidTrack: true, isPitlane: false, sinHeight: 0, sinLength: 0, vibrationGain: 0, vibrationLength: 0, wav: "road.wav", wavPitch: 1, ffEffect: "" }
  ] };
}

test("applies and restores all surface edit fields from a stable baseline", () => {
  const source = config(), baseline = captureSurfaceBaseline(source), edits = { "0": {
    key: "TARMAC", friction: 1.05, damping: 0.02, dirtAdditive: 0.1,
    blackFlagTime: 3, isValidTrack: false, isPitlane: true, sinHeight: 0.001,
    sinLength: 2.5, vibrationGain: 0.15, vibrationLength: 0.4,
    wav: null, wavPitch: 1.2, ffEffect: "GRAIN"
  } };
  assert.equal(surfaceEditCount(edits), 14);
  assert.equal(applySurfaceEdits(source, edits, baseline), 14);
  assert.deepEqual(source.surfaces[0], { index: 0, key: "TARMAC", friction: 1.05, damping: 0.02, dirtAdditive: 0.1, blackFlagTime: 3, isValidTrack: false, isPitlane: true, sinHeight: 0.001, sinLength: 2.5, vibrationGain: 0.15, vibrationLength: 0.4, wav: "", wavPitch: 1.2, ffEffect: "GRAIN" });
  assert.equal(applySurfaceEdits(source, {}, baseline), 0);
  assert.deepEqual(source, config());
});
