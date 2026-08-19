import assert from "node:assert/strict";
import test from "node:test";
import { cspPointInOccluderPolygon, cspTrackOccluded, cspTrackOccluderBlocks } from "../src/csp-occlusion.js";

test("culls a target behind a CSP vertical wall only below its authored top", () => {
  const wall = { type: "wall", culling: true, points: [[-2, 5, 0], [2, 7, 0]], exclusion: [] };
  assert.equal(cspTrackOccluderBlocks(wall, [0, 1, -4], [0, 1, 4]), true);
  assert.equal(cspTrackOccluderBlocks(wall, [0, 10, -4], [0, 10, 4]), false);
  assert.equal(cspTrackOccluderBlocks(wall, [4, 1, -4], [4, 1, 4]), false);
});

test("culls through CSP box sides and honors camera exclusion polygons", () => {
  const box = { type: "box", culling: true, points: [[-2, 5, -1], [2, 5, -1], [2, 5, 1], [-2, 5, 1]], exclusion: [] };
  assert.equal(cspTrackOccluded([box], [0, 1, -4], [0, 1, 4]), true);
  assert.equal(cspTrackOccluded([box], [0, 8, -4], [0, 8, 4]), false);
  box.exclusion = [[-1, 0, -5], [1, 0, -5], [1, 0, -3], [-1, 0, -3]];
  assert.equal(cspPointInOccluderPolygon([0, 1, -4], box.exclusion), true);
  assert.equal(cspTrackOccluded([box], [0, 1, -4], [0, 1, 4]), false);
});
