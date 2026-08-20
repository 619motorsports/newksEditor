import { modelPlacementMatrix } from "./kn5-workspace.js";

export const WORKSPACE_FILE_EDIT_KEYS = [
  "name", "position", "rotation", "lodIn", "lodOut",
  "probability", "multiplicity", "posMode", "positionCenter", "positionRange",
  "velMode", "velocityBase", "velocityRange", "playWav"
];

function cloneVector(value, fallback, length = fallback.length) {
  const output = Array.from(value || fallback, Number);
  return output.length === length && output.every(Number.isFinite) ? output : [...fallback];
}

function cloneDynamic(value) {
  if (!value) return null;
  return {
    ...value,
    probability: Number(value.probability ?? 100),
    multiplicity: cloneVector(value.multiplicity, [1, 1], 2),
    posMode: String(value.posMode || "RANDOM"),
    positionCenter: cloneVector(value.positionCenter, [0, 0, 0]),
    positionRange: cloneVector(value.positionRange, [0, 0, 0]),
    velMode: String(value.velMode || "RANDOM"),
    velocityBase: cloneVector(value.velocityBase, [0, 0, 0]),
    velocityRange: cloneVector(value.velocityRange, [0, 0, 0]),
    playWav: String(value.playWav || "")
  };
}

export function captureWorkspaceBaseline(model) {
  const workspace = model?.workspace;
  if (!workspace) return null;
  return {
    cockpitHrDistance: workspace.cockpitHrDistance,
    driverHrDistance: workspace.driverHrDistance,
    files: workspace.files.map((file) => ({
      name: String(file.name || ""),
      position: cloneVector(file.position, [0, 0, 0]),
      rotation: cloneVector(file.rotation, [0, 0, 0]),
      lod: file.lod ? { ...file.lod } : null,
      dynamic: cloneDynamic(file.dynamic)
    }))
  };
}

export function workspaceEditCount(edits) {
  const files = Object.values(edits?.files || {}).reduce((count, edit) => count + WORKSPACE_FILE_EDIT_KEYS.filter((key) => edit?.[key] !== undefined).length, 0);
  return files + ["cockpitHrDistance", "driverHrDistance"].filter((key) => edits?.[key] !== undefined).length;
}

export function applyWorkspaceEdits(model, edits = {}, baseline) {
  const workspace = model?.workspace;
  if (!workspace || !baseline) return 0;
  workspace.cockpitHrDistance = baseline.cockpitHrDistance;
  workspace.driverHrDistance = baseline.driverHrDistance;
  for (let index = 0; index < workspace.files.length; index++) {
    const file = workspace.files[index], source = baseline.files[index], root = model.root.children[index];
    if (!source) continue;
    file.name = source.name; file.position = [...source.position]; file.rotation = [...source.rotation]; file.dynamic = cloneDynamic(source.dynamic);
    if (file.lod && source.lod) { file.lod.in = source.lod.in; file.lod.out = source.lod.out; }
    if (root) {
      root.transform = modelPlacementMatrix(file.position, file.rotation);
      if (root.workspaceLod && source.lod) { root.workspaceLod.in = source.lod.in; root.workspaceLod.out = source.lod.out; }
    }
  }
  for (const [key, edit] of Object.entries(edits.files || {})) {
    const index = Number(key), file = workspace.files[index], root = model.root.children[index];
    if (!Number.isInteger(index) || !file || !root) continue;
    if (file.dynamic) {
      for (const vector of ["multiplicity", "positionCenter", "positionRange", "velocityBase", "velocityRange"]) if (edit[vector] !== undefined) file.dynamic[vector] = [...edit[vector]];
      for (const scalar of ["probability", "posMode", "velMode"]) if (edit[scalar] !== undefined) file.dynamic[scalar] = edit[scalar];
      if (edit.playWav !== undefined) file.dynamic.playWav = edit.playWav || "";
      file.position = [...file.dynamic.positionCenter];
    } else {
      if (edit.position) file.position = [...edit.position];
      if (edit.rotation) file.rotation = [...edit.rotation];
    }
    root.transform = modelPlacementMatrix(file.position, file.rotation);
    if (file.lod && edit.name !== undefined) file.name = edit.name;
    if (file.lod && root.workspaceLod) {
      if (edit.lodIn !== undefined) file.lod.in = root.workspaceLod.in = edit.lodIn;
      if (edit.lodOut !== undefined) file.lod.out = root.workspaceLod.out = edit.lodOut;
    }
  }
  for (const key of ["cockpitHrDistance", "driverHrDistance"]) if (edits[key] !== undefined) workspace[key] = edits[key];
  return workspaceEditCount(edits);
}
