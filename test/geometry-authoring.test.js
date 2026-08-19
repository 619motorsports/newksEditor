import assert from "node:assert/strict";
import test from "node:test";
import { applyGeometryEdits, captureStaticGeometryBaselines, staticGeometryMetrics, transformStaticGeometry } from "../src/geometry-authoring.js";
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

test("restores baselines before reapplying stable-path geometry edits", () => {
  const node = mesh(), root = { kind: "node", name: "ROOT", active: true, transform: identity, children: [node] }, baselines = captureStaticGeometryBaselines(root), warnings = [];
  const edit = { "0": { transform: composeNodeTransform({ position: [1, 0, 0], rotation: [0, 0, 0], scale: [1, 1, 1] }) } };
  assert.equal(applyGeometryEdits(root, edit, baselines, warnings), 1);
  assert.equal(node.vertices[0], 1);
  assert.equal(applyGeometryEdits(root, edit, baselines, warnings), 1);
  assert.equal(node.vertices[0], 1);
  assert.equal(applyGeometryEdits(root, {}, baselines, warnings), 0);
  assert.equal(node.vertices[0], 0);
  assert.deepEqual(warnings, []);
  assert.deepEqual(staticGeometryMetrics(node).size, [2, 2, 0]);
});

test("rejects collapsed transforms and protects skinned bind-pose geometry", () => {
  const source = mesh(), collapsed = composeNodeTransform({ position: [0, 0, 0], rotation: [0, 0, 0], scale: [0, 1, 1] });
  assert.throws(() => transformStaticGeometry(source, collapsed), /cannot collapse/);
  const skinned = { ...source, kind: "skinnedMesh", vertexStride: 19, vertices: new Float32Array(19), bones: [] }, root = { kind: "node", name: "ROOT", active: true, transform: identity, children: [skinned] }, warnings = [];
  assert.equal(applyGeometryEdits(root, { "0": { transform: identity } }, null, warnings), 0);
  assert.match(warnings[0], /skinned bind-pose geometry/);
});
