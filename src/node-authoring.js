import { decomposeKsAnimationMatrix, ksAnimationMatrix } from "./ksanim.js";

export const ROOT_NODE_PATH = "root";

export function nodePathEntries(root) {
  const entries = [];
  const visit = (node, path) => {
    entries.push({ node, path });
    for (let index = 0; index < (node.children || []).length; index++) {
      visit(node.children[index], path === ROOT_NODE_PATH ? String(index) : `${path}/${index}`);
    }
  };
  if (root) visit(root, ROOT_NODE_PATH);
  return entries;
}

export function nodeAtPath(root, path) {
  if (!root || path === ROOT_NODE_PATH) return path === ROOT_NODE_PATH ? root : null;
  if (!/^\d+(?:\/\d+)*$/.test(String(path))) return null;
  let node = root;
  for (const part of String(path).split("/")) {
    node = node?.children?.[Number(part)];
    if (!node) return null;
  }
  return node;
}

function finiteVector(value, length, label) {
  const result = Array.from(value || [], Number);
  if (result.length !== length || result.some((item) => !Number.isFinite(item))) throw new TypeError(`${label} needs ${length} finite numbers`);
  return result;
}

function normalizedQuaternion(value) {
  const length = Math.hypot(...value);
  return length > Number.EPSILON ? value.map((component) => component / length) : [0, 0, 0, 1];
}

export function quaternionFromEulerDegrees(rotation) {
  const [x, y, z] = finiteVector(rotation, 3, "Rotation").map((value) => value * Math.PI / 180);
  const [sx, sy, sz] = [Math.sin(x / 2), Math.sin(y / 2), Math.sin(z / 2)];
  const [cx, cy, cz] = [Math.cos(x / 2), Math.cos(y / 2), Math.cos(z / 2)];
  return normalizedQuaternion([
    sx * cy * cz - cx * sy * sz,
    cx * sy * cz + sx * cy * sz,
    cx * cy * sz - sx * sy * cz,
    cx * cy * cz + sx * sy * sz
  ]);
}

export function eulerDegreesFromQuaternion(quaternion) {
  const [x, y, z, w] = normalizedQuaternion(finiteVector(quaternion, 4, "Quaternion"));
  const sinPitch = Math.max(-1, Math.min(1, 2 * (w * y - z * x)));
  return [
    Math.atan2(2 * (w * x + y * z), 1 - 2 * (x * x + y * y)),
    Math.asin(sinPitch),
    Math.atan2(2 * (w * z + x * y), 1 - 2 * (y * y + z * z))
  ].map((value) => value * 180 / Math.PI);
}

export function composeNodeTransform({ position, rotation, scale }) {
  return ksAnimationMatrix({
    position: finiteVector(position, 3, "Position"),
    quaternion: quaternionFromEulerDegrees(rotation),
    scale: finiteVector(scale, 3, "Scale")
  });
}

export function decomposeNodeTransform(matrix) {
  const source = finiteVector(matrix, 16, "Transform"), frame = decomposeKsAnimationMatrix(source);
  const result = { position: frame.position, rotation: eulerDegreesFromQuaternion(frame.quaternion), scale: frame.scale };
  const rebuilt = composeNodeTransform(result), error = Math.max(...source.map((value, index) => Math.abs(value - rebuilt[index])));
  return { ...result, decomposable: error < 1e-4, error };
}

export function applyNodeEdits(root, edits, warnings = []) {
  let applied = 0;
  for (const [path, edit] of Object.entries(edits || {})) {
    const node = nodeAtPath(root, path);
    if (!node) { warnings.push(`${path}: hierarchy node was not found`); continue; }
    let changed = false;
    if (typeof edit.name === "string" && edit.name) { node.name = edit.name; changed = true; }
    if (typeof edit.active === "boolean") { node.active = edit.active; changed = true; }
    if (edit.transform) {
      if (node.kind !== "node") warnings.push(`${path}: ${node.name} cannot store a local transform`);
      else { node.transform = [...edit.transform]; changed = true; }
    }
    if (changed) applied++;
  }
  return applied;
}
