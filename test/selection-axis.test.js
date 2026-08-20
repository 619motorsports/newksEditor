import assert from "node:assert/strict";
import test from "node:test";
import { NATIVE_SELECTION_AXIS_COLORS, NATIVE_SELECTION_AXIS_LENGTH, nativeSelectionAxis } from "../src/selection-axis.js";

const identity = [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1];

test("builds the native one-meter selected-node axes", () => {
  const axis = nativeSelectionAxis(identity);
  assert.equal(axis.valid, true);
  assert.equal(NATIVE_SELECTION_AXIS_LENGTH, 1);
  assert.deepEqual(NATIVE_SELECTION_AXIS_COLORS, [[1, 0, 0], [0, 1, 0], [0, 0, 1]]);
  assert.deepEqual(axis.origin, [0, 0, 0]);
  assert.deepEqual(axis.directions, [[1, 0, 0], [0, 1, 0], [0, 0, -1]]);
  assert.deepEqual([...axis.vertices], [
    0, 0, 0, 1, 0, 0, 1, 0, 0, 1, 0, 0,
    0, 0, 0, 0, 1, 0, 0, 1, 0, 0, 1, 0,
    0, 0, 0, 0, 0, 1, 0, 0, -1, 0, 0, 1
  ]);
});

test("normalizes transformed world bases and keeps the world origin", () => {
  const axis = nativeSelectionAxis([
    0, 2, 0, 0,
    -3, 0, 0, 0,
    0, 0, 4, 0,
    10, 20, 30, 1
  ]);
  assert.equal(axis.valid, true);
  assert.deepEqual(axis.origin, [10, 20, 30]);
  assert.deepEqual(axis.directions, [[0, 1, 0], [-1, 0, 0], [0, 0, -1]]);
  assert.deepEqual([...axis.vertices.slice(0, 12)], [10, 20, 30, 1, 0, 0, 10, 21, 30, 1, 0, 0]);
  assert.deepEqual([...axis.vertices.slice(-12)], [10, 20, 30, 0, 0, 1, 10, 20, 29, 0, 0, 1]);
});

test("rejects truncated, non-finite, and collapsed world transforms", () => {
  assert.match(nativeSelectionAxis(identity.slice(0, 15)).warning, /16 numbers/);
  assert.match(nativeSelectionAxis([...identity.slice(0, 14), Number.NaN, 1]).warning, /non-finite/);
  assert.match(nativeSelectionAxis([0, 0, 0, 0, ...identity.slice(4)]).warning, /zero-length/);
});
