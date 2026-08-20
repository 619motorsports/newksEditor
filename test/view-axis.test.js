import assert from "node:assert/strict";
import test from "node:test";
import {
  NATIVE_VIEW_AXIS_COLORS,
  NATIVE_VIEW_AXIS_LENGTH,
  NATIVE_VIEW_AXIS_MODE_AFTER_3D,
  NATIVE_VIEW_AXIS_MODE_NONE,
  nativeViewAxisToggle,
  nativeViewAxisVertices
} from "../src/view-axis.js";

test("toggles the native view axis between none and after-3D modes", () => {
  assert.equal(nativeViewAxisToggle(NATIVE_VIEW_AXIS_MODE_NONE), NATIVE_VIEW_AXIS_MODE_AFTER_3D);
  assert.equal(nativeViewAxisToggle(NATIVE_VIEW_AXIS_MODE_AFTER_3D), NATIVE_VIEW_AXIS_MODE_NONE);
  assert.equal(nativeViewAxisToggle(1), NATIVE_VIEW_AXIS_MODE_AFTER_3D);
});

test("builds the native one-meter world-origin view axis", () => {
  const vertices = nativeViewAxisVertices();
  assert.equal(NATIVE_VIEW_AXIS_LENGTH, 1);
  assert.deepEqual(NATIVE_VIEW_AXIS_COLORS, [[3, 0, 0], [0, 3, 0], [0, 0, 3]]);
  assert.deepEqual([...vertices], [
    0, 0, 0, 3, 0, 0, 1, 0, 0, 3, 0, 0,
    0, 0, 0, 0, 3, 0, 0, 1, 0, 0, 3, 0,
    0, 0, 0, 0, 0, 3, 0, 0, 1, 0, 0, 3
  ]);
});
