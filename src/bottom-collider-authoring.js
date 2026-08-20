export const BOTTOM_COLLIDER_EDIT_KEYS = ["centre", "size", "groundEnabled"];

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
