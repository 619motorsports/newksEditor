export const NATIVE_VIEW_AXIS_MODE_NONE = 0;
export const NATIVE_VIEW_AXIS_MODE_AFTER_3D = 2;
export const NATIVE_VIEW_AXIS_LENGTH = 1;
export const NATIVE_VIEW_AXIS_COLORS = Object.freeze([
  Object.freeze([3, 0, 0]),
  Object.freeze([0, 3, 0]),
  Object.freeze([0, 0, 3])
]);

export function nativeViewAxisToggle(mode) {
  return Number(mode) === NATIVE_VIEW_AXIS_MODE_AFTER_3D
    ? NATIVE_VIEW_AXIS_MODE_NONE
    : NATIVE_VIEW_AXIS_MODE_AFTER_3D;
}

/** Build the world-origin axis that the native editor draws after opaque geometry. */
export function nativeViewAxisVertices() {
  const directions = [[1, 0, 0], [0, 1, 0], [0, 0, 1]], vertices = [];
  for (let axis = 0; axis < directions.length; axis++) {
    const direction = directions[axis], color = NATIVE_VIEW_AXIS_COLORS[axis];
    vertices.push(0, 0, 0, ...color);
    vertices.push(...direction.map((value) => value * NATIVE_VIEW_AXIS_LENGTH), ...color);
  }
  return new Float32Array(vertices);
}
