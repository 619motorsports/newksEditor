import assert from "node:assert/strict";
import test from "node:test";
import { applyGeometryEdits, captureStaticGeometryBaselines, repairStaticTopology, staticGeometryMetrics, transformStaticGeometry } from "../src/geometry-authoring.js";
import { composeNodeTransform } from "../src/node-authoring.js";

const identity = [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1];

function packedTangent(x, y, z, marker = 0x5a) {
  const view = new DataView(new ArrayBuffer(4));
  view.setUint32(0, (marker << 24) | (Math.round((x + 1) * .5 * 255) & 255) | ((Math.round((y + 1) * .5 * 255) & 255) << 8) | ((Math.round((z + 1) * .5 * 255) & 255) << 16), true);
  return view.getFloat32(0, true);
}

function tangentBits(vertices, offset = 8) {
  return new DataView(vertices.buffer, vertices.byteOffset, vertices.byteLength).getUint32(offset * 4, true);
}

function mesh() {
  const vertices = new Float32Array(33), points = [[0, 0, 0], [2, 0, 0], [0, 2, 0]];
  for (let index = 0; index < points.length; index++) {
    const offset = index * 11;
    vertices.set([...points[index], 0, 0, 1, index / 2, index / 3], offset);
    vertices[offset + 8] = packedTangent(1, 0, 0);
    vertices[offset + 9] = 17; vertices[offset + 10] = 23;
  }
  return { type: 2, kind: "mesh", name: "BODY", active: true, visible: true, renderable: true, castShadows: true, transparent: false, vertexStride: 11, vertices, indices: new Uint16Array([0, 1, 2]), materialId: 0, layer: 0, lodIn: 0, lodOut: 100, bounds: [1, 1, 0, Math.SQRT2], children: [] };
}

test("transforms static positions around their bounds center and preserves other channels", () => {
  const source = mesh(), uv = Array.from(source.vertices.slice(6, 11)), transform = composeNodeTransform({ position: [3, 4, 5], rotation: [0, 0, 0], scale: [2, 1, 1] });
  const result = transformStaticGeometry(source, transform);
  assert.deepEqual(Array.from(result.vertices.slice(0, 3)), [-1 + 3, 4, 5]);
  assert.deepEqual(Array.from(result.vertices.slice(11, 14)), [3 + 3, 4, 5]);
  assert.deepEqual(Array.from(result.vertices.slice(6, 8)), uv.slice(0, 2));
  assert.deepEqual(Array.from(result.vertices.slice(9, 11)), uv.slice(3, 5));
  assert.deepEqual(result.bounds.slice(0, 3), [4, 5, 5]);
  assert.ok(Math.abs(result.bounds[3] - Math.sqrt(5)) < 1e-6);
});

test("rotates normals and packed tangents while preserving the tangent marker byte", () => {
  const source = mesh(), transform = composeNodeTransform({ position: [0, 0, 0], rotation: [0, 90, 0], scale: [1, 1, 1] }), result = transformStaticGeometry(source, transform);
  assert.ok(result.vertices[3] > .9999);
  assert.ok(Math.abs(result.vertices[5]) < 1e-6);
  const packed = tangentBits(result.vertices);
  assert.equal(packed >>> 24, 0x5a);
  const x = (packed & 255) / 255 * 2 - 1, z = ((packed >>> 16) & 255) / 255 * 2 - 1;
  assert.ok(Math.abs(x) < .01);
  assert.ok(z < -.99);
});

test("rotates ordinary three-float tangents from imported geometry", () => {
  const source = mesh();
  for (let offset = 0; offset < source.vertices.length; offset += source.vertexStride) source.vertices.set([1, 0, 0], offset + 8);
  const transform = composeNodeTransform({ position: [0, 0, 0], rotation: [0, 90, 0], scale: [1, 1, 1] }), result = transformStaticGeometry(source, transform);
  assert.ok(Math.abs(result.vertices[8]) < 1e-6);
  assert.ok(Math.abs(result.vertices[9]) < 1e-6);
  assert.ok(result.vertices[10] < -.9999);
});

test("removes degenerate triangles and rebuilds area-weighted normals", () => {
  const source = mesh();
  source.indices = new Uint16Array([0, 1, 2, 0, 0, 1]);
  for (let offset = 0; offset < source.vertices.length; offset += source.vertexStride) source.vertices.set([0, 1, 0], offset + 3);
  const result = repairStaticTopology(source, { removeDegenerate: true, recalculateNormals: true });
  assert.deepEqual(Array.from(result.indices), [0, 1, 2]);
  assert.equal(result.removedTriangles, 1);
  for (let offset = 0; offset < result.vertices.length; offset += source.vertexStride) assert.deepEqual(Array.from(result.vertices.slice(offset + 3, offset + 6)), [0, 0, 1]);
});

test("reverses triangle winding and source normals together", () => {
  const source = mesh(), result = repairStaticTopology(source, { reverseWinding: true });
  assert.deepEqual(Array.from(result.indices), [0, 2, 1]);
  for (let offset = 0; offset < result.vertices.length; offset += source.vertexStride) {
    assert.ok(Math.abs(result.vertices[offset + 3]) < 1e-8); assert.ok(Math.abs(result.vertices[offset + 4]) < 1e-8); assert.equal(result.vertices[offset + 5], -1);
  }
});

test("restores baselines before reapplying stable-path geometry edits", () => {
  const node = mesh(); node.indices = new Uint16Array([0, 1, 2, 0, 0, 1]);
  const root = { kind: "node", name: "ROOT", active: true, transform: identity, children: [node] }, baselines = captureStaticGeometryBaselines(root), warnings = [];
  const edit = { "0": { transform: composeNodeTransform({ position: [1, 0, 0], rotation: [0, 0, 0], scale: [1, 1, 1] }), removeDegenerate: true } };
  assert.equal(applyGeometryEdits(root, edit, baselines, warnings), 1);
  assert.equal(node.vertices[0], 1);
  assert.equal(node.indices.length, 3);
  assert.equal(applyGeometryEdits(root, edit, baselines, warnings), 1);
  assert.equal(node.vertices[0], 1);
  assert.equal(node.indices.length, 3);
  assert.equal(applyGeometryEdits(root, {}, baselines, warnings), 0);
  assert.equal(node.vertices[0], 0);
  assert.equal(node.indices.length, 6);
  assert.deepEqual(warnings, []);
  assert.deepEqual(staticGeometryMetrics(node).size, [2, 2, 0]);
});

test("reverses winding after a mirrored geometry transform", () => {
  const node = mesh(), root = { kind: "node", name: "ROOT", active: true, transform: identity, children: [node] }, baselines = captureStaticGeometryBaselines(root), warnings = [];
  const transform = composeNodeTransform({ position: [0, 0, 0], rotation: [0, 0, 0], scale: [-1, 1, 1] });
  assert.equal(applyGeometryEdits(root, { "0": { transform } }, baselines, warnings), 1);
  assert.deepEqual(Array.from(node.indices), [0, 2, 1]);
  const a = node.indices[0] * node.vertexStride, b = node.indices[1] * node.vertexStride, c = node.indices[2] * node.vertexStride;
  const ab = [node.vertices[b] - node.vertices[a], node.vertices[b + 1] - node.vertices[a + 1], node.vertices[b + 2] - node.vertices[a + 2]];
  const ac = [node.vertices[c] - node.vertices[a], node.vertices[c + 1] - node.vertices[a + 1], node.vertices[c + 2] - node.vertices[a + 2]];
  const faceZ = ab[0] * ac[1] - ab[1] * ac[0];
  assert.ok(faceZ > 0);
  assert.ok(node.vertices[a + 5] > 0);
  assert.deepEqual(warnings, []);
});

test("rejects collapsed transforms and protects skinned bind-pose geometry", () => {
  const source = mesh(), collapsed = composeNodeTransform({ position: [0, 0, 0], rotation: [0, 0, 0], scale: [0, 1, 1] });
  assert.throws(() => transformStaticGeometry(source, collapsed), /cannot collapse/);
  const skinned = { ...source, kind: "skinnedMesh", vertexStride: 19, vertices: new Float32Array(19), bones: [] }, root = { kind: "node", name: "ROOT", active: true, transform: identity, children: [skinned] }, warnings = [];
  assert.equal(applyGeometryEdits(root, { "0": { transform: identity } }, null, warnings), 0);
  assert.match(warnings[0], /skinned bind-pose geometry/);
});
