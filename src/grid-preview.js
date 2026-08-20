export const NATIVE_GRID_HALF_EXTENT = 5;
export const NATIVE_GRID_STEP = 1;
export const NATIVE_GRID_COLOR = Object.freeze([1, 0, 1]);

export function nativeGridToggle(visible) {
  return !Boolean(visible);
}

/** Build the 22 line segments emitted by the native editor. */
export function nativeGridVertices() {
  const vertices = [];
  for (let coordinate = -NATIVE_GRID_HALF_EXTENT; coordinate <= NATIVE_GRID_HALF_EXTENT; coordinate += NATIVE_GRID_STEP) {
    const x = -coordinate;
    vertices.push(x, 0, -NATIVE_GRID_HALF_EXTENT, x, 0, NATIVE_GRID_HALF_EXTENT);
  }
  for (let coordinate = -NATIVE_GRID_HALF_EXTENT; coordinate <= NATIVE_GRID_HALF_EXTENT; coordinate += NATIVE_GRID_STEP) {
    const z = -coordinate;
    vertices.push(-NATIVE_GRID_HALF_EXTENT, 0, z, NATIVE_GRID_HALF_EXTENT, 0, z);
  }
  return new Float32Array(vertices);
}
