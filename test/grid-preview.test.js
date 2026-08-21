import assert from "node:assert/strict";
import test from "node:test";
import { NATIVE_GRID_COLOR, NATIVE_GRID_HALF_EXTENT, NATIVE_GRID_STEP, nativeGridToggle, nativeGridVertices } from "../src/grid-preview.js";

test("inverts the native grid visibility state", () => {
  assert.equal(nativeGridToggle(false), true);
  assert.equal(nativeGridToggle(true), false);
});

test("builds the native 11 by 11-line magenta grid on the XZ plane", () => {
  const vertices = nativeGridVertices();
  assert.equal(NATIVE_GRID_HALF_EXTENT, 5);
  assert.equal(NATIVE_GRID_STEP, 1);
  assert.deepEqual(NATIVE_GRID_COLOR, [1, 0, 1]);
  assert.equal(vertices.length, 22 * 2 * 3);
  assert.deepEqual([...vertices.slice(0, 6)], [5, 0, -5, 5, 0, 5]);
  assert.deepEqual([...vertices.slice(60, 72)], [-5, 0, -5, -5, 0, 5, -5, 0, 5, 5, 0, 5]);
  assert.deepEqual([...vertices.slice(66, 78)], [-5, 0, 5, 5, 0, 5, -5, 0, 4, 5, 0, 4]);
  assert.deepEqual([...vertices.slice(-6)], [-5, 0, -5, 5, 0, -5]);
  for (let index = 1; index < vertices.length; index += 3) assert.equal(vertices[index], 0);
  for (const value of vertices) assert.ok(value >= -5 && value <= 5);
});
