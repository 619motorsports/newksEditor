export const NATIVE_SELECTION_AXIS_LENGTH = 1;
export const NATIVE_SELECTION_AXIS_COLORS = Object.freeze([
  Object.freeze([1, 0, 0]),
  Object.freeze([0, 1, 0]),
  Object.freeze([0, 0, 1])
]);

function invalidAxis(warning) {
  return { valid: false, warning, origin: [], directions: [], vertices: new Float32Array() };
}

function normalized(value) {
  const length = Math.hypot(...value);
  return length > 1e-12 ? value.map((component) => component === 0 ? 0 : component / length) : null;
}

/** Build the three one-meter world-axis segments emitted for the selected native node. */
export function nativeSelectionAxis(world) {
  if (!world || typeof world.length !== "number" || world.length < 16) {
    return invalidAxis("The selected node world transform must contain 16 numbers.");
  }
  const matrix = Array.from({ length: 16 }, (_, index) => Number(world[index]));
  if (matrix.some((value) => !Number.isFinite(value))) {
    return invalidAxis("The selected node world transform contains a non-finite number.");
  }
  const origin = [matrix[12], matrix[13], matrix[14]];
  const directions = [
    normalized([matrix[0], matrix[1], matrix[2]]),
    normalized([matrix[4], matrix[5], matrix[6]]),
    normalized([-matrix[8], -matrix[9], -matrix[10]])
  ];
  if (directions.some((direction) => !direction)) {
    return invalidAxis("The selected node world transform contains a zero-length axis.");
  }
  const vertices = [];
  for (let axis = 0; axis < directions.length; axis++) {
    const direction = directions[axis], color = NATIVE_SELECTION_AXIS_COLORS[axis];
    vertices.push(...origin, ...color);
    vertices.push(...origin.map((value, index) => value + direction[index] * NATIVE_SELECTION_AXIS_LENGTH), ...color);
  }
  return { valid: true, warning: "", origin, directions, vertices: new Float32Array(vertices) };
}
