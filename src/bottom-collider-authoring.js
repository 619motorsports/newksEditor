export const BOTTOM_COLLIDER_EDIT_KEYS = ["centre", "size", "groundEnabled"];
const FLOAT32_MAXIMUM = 3.4028234663852886e38;

export function validateBottomColliderVector(value, key) {
  const label = key === "centre" ? "Centre" : "Size";
  if (!Array.isArray(value) || value.length !== 3) throw new Error(`${label} needs three finite numbers`);
  const vector = value.map(Number);
  if (vector.some((component) => !Number.isFinite(component))) throw new Error(`${label} needs three finite numbers`);
  if (vector.some((component) => Math.abs(component) > FLOAT32_MAXIMUM)) throw new Error(`${label} components must fit a finite float32 value`);
  if (key === "size" && vector.some((component) => component <= 0 || Math.fround(component) <= 0)) throw new Error("Size components must be positive float32 values");
  return vector;
}

function cloneCollider(collider) {
  const centre = [...collider.centre], size = [...collider.size];
  return {
    ...collider,
    centre,
    size,
    bounds: collider.bounds ? { min: [...collider.bounds.min], max: [...collider.bounds.max] } : {
      min: centre.map((value, axis) => value - size[axis] / 2),
      max: centre.map((value, axis) => value + size[axis] / 2)
    }
  };
}

export function captureBottomColliderBaseline(config) {
  return config?.colliders ? config.colliders.map(cloneCollider) : null;
}

export function bottomColliderEditCount(edits) {
  return Object.values(edits || {}).reduce((count, edit) => count + BOTTOM_COLLIDER_EDIT_KEYS.filter((key) => edit?.[key] !== undefined).length, 0);
}

export function applyBottomColliderEdits(config, edits = {}, baseline) {
  if (!config || !baseline) return 0;
  config.colliders = baseline.map(cloneCollider);
  let applied = 0;
  for (const [key, edit] of Object.entries(edits || {})) {
    const position = Number(key), collider = config.colliders[position];
    if (!Number.isInteger(position) || !collider || !edit) continue;
    if (edit.centre !== undefined) { collider.centre = [...edit.centre]; applied++; }
    if (edit.size !== undefined) { collider.size = [...edit.size]; applied++; }
    if (edit.groundEnabled !== undefined) { collider.groundEnabled = edit.groundEnabled; applied++; }
    collider.bounds = {
      min: collider.centre.map((value, axis) => value - collider.size[axis] / 2),
      max: collider.centre.map((value, axis) => value + collider.size[axis] / 2)
    };
  }
  return applied;
}
