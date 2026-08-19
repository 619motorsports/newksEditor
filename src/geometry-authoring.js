import { nodeAtPath, nodePathEntries } from "./node-authoring.js";

const IDENTITY = [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1];

function cloneFloat32(value) {
  const bytes = new Uint8Array(value.buffer, value.byteOffset, value.byteLength).slice();
  return new Float32Array(bytes.buffer);
}

function matrix(value) {
  const result = Array.from(value || IDENTITY, Number);
  if (result.length !== 16 || result.some((item) => !Number.isFinite(item))) throw new TypeError("Geometry transform needs 16 finite numbers");
  return result;
}

function normalize(value, fallback = [0, 1, 0]) {
  const length = Math.hypot(...value);
  return length > 1e-8 ? value.map((component) => component / length) : [...fallback];
}

function localBounds(vertices, stride) {
  const minimum = [Infinity, Infinity, Infinity], maximum = [-Infinity, -Infinity, -Infinity];
  for (let offset = 0; offset < vertices.length; offset += stride) {
    for (let axis = 0; axis < 3; axis++) {
      minimum[axis] = Math.min(minimum[axis], vertices[offset + axis]);
      maximum[axis] = Math.max(maximum[axis], vertices[offset + axis]);
    }
  }
  if (minimum[0] === Infinity) return { minimum: [0, 0, 0], maximum: [0, 0, 0], center: [0, 0, 0], size: [0, 0, 0], radius: 0 };
  const center = minimum.map((value, axis) => (value + maximum[axis]) / 2), size = maximum.map((value, axis) => value - minimum[axis]);
  let radius = 0;
  for (let offset = 0; offset < vertices.length; offset += stride) radius = Math.max(radius, Math.hypot(vertices[offset] - center[0], vertices[offset + 1] - center[1], vertices[offset + 2] - center[2]));
  return { minimum, maximum, center, size, radius };
}

function inverseTranspose3(transform) {
  const a = transform[0], b = transform[4], c = transform[8];
  const d = transform[1], e = transform[5], f = transform[9];
  const g = transform[2], h = transform[6], i = transform[10];
  const A = e * i - f * h, B = f * g - d * i, C = d * h - e * g;
  const determinant = a * A + b * B + c * C;
  if (Math.abs(determinant) < 1e-8) throw new TypeError("Geometry scale cannot collapse an axis");
  const inverse = [A, c * h - b * i, b * f - c * e, B, a * i - c * g, c * d - a * f, C, b * g - a * h, a * e - b * d].map((value) => value / determinant);
  return [inverse[0], inverse[3], inverse[6], inverse[1], inverse[4], inverse[7], inverse[2], inverse[5], inverse[8]];
}

function direction(transform, value) {
  return [
    transform[0] * value[0] + transform[4] * value[1] + transform[8] * value[2],
    transform[1] * value[0] + transform[5] * value[1] + transform[9] * value[2],
    transform[2] * value[0] + transform[6] * value[1] + transform[10] * value[2]
  ];
}

function normalDirection(normalMatrix, value) {
  return normalize([
    normalMatrix[0] * value[0] + normalMatrix[1] * value[1] + normalMatrix[2] * value[2],
    normalMatrix[3] * value[0] + normalMatrix[4] * value[1] + normalMatrix[5] * value[2],
    normalMatrix[6] * value[0] + normalMatrix[7] * value[1] + normalMatrix[8] * value[2]
  ]);
}

function unpackTangent(view, byteOffset) {
  const packed = view.getUint32(byteOffset, true);
  return { packed, value: [(packed & 255) / 255 * 2 - 1, ((packed >>> 8) & 255) / 255 * 2 - 1, ((packed >>> 16) & 255) / 255 * 2 - 1] };
}

function packTangent(view, byteOffset, value, previous) {
  const bytes = normalize(value, [1, 0, 0]).map((component) => Math.max(0, Math.min(255, Math.round((component + 1) * 0.5 * 255))));
  view.setUint32(byteOffset, (previous & 0xff000000) | bytes[0] | (bytes[1] << 8) | (bytes[2] << 16), true);
}

export function staticGeometryMetrics(node) {
  if (node?.kind !== "mesh" || node.vertexStride !== 11) return null;
  return { ...localBounds(node.vertices, node.vertexStride), vertices: node.vertices.length / node.vertexStride, triangles: node.indices.length / 3 };
}

export function transformStaticGeometry(node, transformValue, baselineVertices = node?.vertices) {
  if (node?.kind !== "mesh" || node.vertexStride !== 11) throw new TypeError("Geometry transforms require a static 11-float KN5 mesh");
  if (!(baselineVertices instanceof Float32Array) || baselineVertices.length !== node.vertices.length) throw new TypeError("Geometry baseline does not match the mesh");
  const transform = matrix(transformValue), normalMatrix = inverseTranspose3(transform), pivot = localBounds(baselineVertices, node.vertexStride).center;
  const vertices = cloneFloat32(baselineVertices), inputView = new DataView(baselineVertices.buffer, baselineVertices.byteOffset, baselineVertices.byteLength), outputView = new DataView(vertices.buffer);
  for (let offset = 0; offset < vertices.length; offset += node.vertexStride) {
    const relative = [baselineVertices[offset] - pivot[0], baselineVertices[offset + 1] - pivot[1], baselineVertices[offset + 2] - pivot[2]], moved = direction(transform, relative);
    vertices[offset] = moved[0] + pivot[0] + transform[12]; vertices[offset + 1] = moved[1] + pivot[1] + transform[13]; vertices[offset + 2] = moved[2] + pivot[2] + transform[14];
    const normal = normalDirection(normalMatrix, [baselineVertices[offset + 3], baselineVertices[offset + 4], baselineVertices[offset + 5]]);
    vertices.set(normal, offset + 3);
    const tangentFloats = [baselineVertices[offset + 8], baselineVertices[offset + 9], baselineVertices[offset + 10]], tangentLength = Math.hypot(...tangentFloats), plainTangent = tangentFloats.every(Number.isFinite) && tangentLength > .5 && tangentLength < 1.5;
    const tangent = plainTangent ? { value: tangentFloats } : unpackTangent(inputView, (offset + 8) * 4), transformed = direction(transform, tangent.value), dot = transformed[0] * normal[0] + transformed[1] * normal[1] + transformed[2] * normal[2], orthogonal = normalize(transformed.map((value, axis) => value - normal[axis] * dot), [1, 0, 0]);
    if (plainTangent) vertices.set(orthogonal, offset + 8); else packTangent(outputView, (offset + 8) * 4, orthogonal, tangent.packed);
  }
  const bounds = localBounds(vertices, node.vertexStride);
  return { vertices, bounds: [...bounds.center, bounds.radius], metrics: bounds };
}

export function captureStaticGeometryBaselines(root) {
  return new Map(nodePathEntries(root).filter(({ node }) => node.kind === "mesh" && node.vertexStride === 11).map(({ node, path }) => [path, { vertices: cloneFloat32(node.vertices), bounds: node.bounds ? [...node.bounds] : null }]));
}

export function applyGeometryEdits(root, edits, baselines = null, warnings = []) {
  if (baselines instanceof Map) for (const [path, baseline] of baselines) {
    const node = nodeAtPath(root, path);
    if (!node) continue;
    node.vertices = cloneFloat32(baseline.vertices);
    if (baseline.bounds) node.bounds = [...baseline.bounds];
  }
  let applied = 0;
  for (const [path, edit] of Object.entries(edits || {})) {
    const node = nodeAtPath(root, path);
    if (!node) { warnings.push(`${path}: geometry node was not found`); continue; }
    if (node.kind === "skinnedMesh") { warnings.push(`${path}: ${node.name} uses skinned bind-pose geometry and was not changed`); continue; }
    if (node.kind !== "mesh" || node.vertexStride !== 11) { warnings.push(`${path}: ${node.name} is not editable static KN5 geometry`); continue; }
    try {
      const baseline = baselines instanceof Map ? baselines.get(path)?.vertices : node.vertices;
      const result = transformStaticGeometry(node, edit.transform, baseline);
      node.vertices = result.vertices; node.bounds = result.bounds; applied++;
    } catch (error) { warnings.push(`${path}: ${error.message}`); }
  }
  return applied;
}
