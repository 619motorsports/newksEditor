import assert from "node:assert/strict";
import test from "node:test";
import { mergeKn5Models, modelPlacementMatrix } from "../src/kn5-workspace.js";
import { analyzeScene } from "../src/scene-diagnostics.js";

function mesh(name, points, options = {}) {
  const stride = options.skinned ? 19 : 11, vertices = new Float32Array(points.length * stride);
  for (let index = 0; index < points.length; index++) vertices.set(points[index], index * stride);
  return {
    kind: options.skinned ? "skinnedMesh" : "mesh", name, active: options.active ?? true,
    visible: options.visible ?? true, renderable: options.renderable ?? true,
    materialId: options.materialId ?? 0, vertexStride: stride, vertices,
    indices: new Uint16Array(options.indices || [0, 1, 2]), children: []
  };
}

function model(name, child, textureSize = 16) {
  return {
    version: 6, byteLength: 200 + textureSize,
    textures: [{ name: `${name}.dds`, size: textureSize, data: new Uint8Array(textureSize) }],
    materials: [{ name: `${name} material`, shader: "ksPerPixel", resources: [] }],
    root: { kind: "node", name, active: true, transform: modelPlacementMatrix(), children: [child] }
  };
}

test("counts scene payloads and transforms exact world bounds", () => {
  const visible = mesh("large", [[0, 0, 0], [2, 0, 0], [0, 4, 0]], { indices: [0, 1, 2, 2, 1, 0] });
  const hidden = mesh("hidden", [[-1, -1, -1], [1, 1, 1], [0, 0, 0]], { visible: false, skinned: true });
  const scene = model("track", { kind: "node", name: "placed", active: true, transform: modelPlacementMatrix([10, 20, 30]), children: [visible, hidden] }, 64);
  const result = analyzeScene(scene, { sourceName: "track.kn5" });
  assert.deepEqual(result.totals, {
    nodes: 4, meshes: 2, visibleMeshes: 1, staticMeshes: 1, skinnedMeshes: 1,
    vertices: 6, triangles: 3, geometryBytes: visible.vertices.byteLength + visible.indices.byteLength + hidden.vertices.byteLength + hidden.indices.byteLength,
    emptyMeshes: 0, invalidMaterialMeshes: 0, maxDepth: 2, materials: 1, textures: 1,
    effectiveTextures: 1, textureBytes: 64, sourceBytes: 264, usedMaterials: 1
  });
  assert.deepEqual(result.bounds.min, [9, 19, 29]);
  assert.deepEqual(result.bounds.max, [12, 24, 31]);
  assert.deepEqual(result.visibleBounds.min, [10, 20, 30]);
  assert.deepEqual(result.visibleBounds.max, [12, 24, 30]);
  assert.equal(result.files.length, 1);
  assert.equal(result.files[0].name, "track.kn5");
  assert.equal(result.files[0].sourceBytes, 264);
  assert.equal(result.largestMeshes[0].name, "large");
  assert.equal(result.largestMeshes[0].triangles, 2);
});

test("attributes geometry and original texture payloads to workspace files", () => {
  const first = model("main", mesh("road", [[0, 0, 0], [1, 0, 0], [0, 0, 1]]), 10);
  const second = model("details", mesh("sign", [[0, 0, 0], [0, 2, 0], [0, 0, 3]], { materialId: 0 }), 20);
  const workspace = mergeKn5Models([
    { name: "main.kn5", size: 500, model: first },
    { name: "details.kn5", size: 700, model: second, position: [100, 0, 0] }
  ], { kind: "track", name: "layout" });
  const result = analyzeScene(workspace);
  assert.equal(result.totals.nodes, 4);
  assert.equal(result.totals.meshes, 2);
  assert.equal(result.totals.sourceBytes, 1200);
  assert.equal(result.totals.textures, 2);
  assert.equal(result.totals.textureBytes, 30);
  assert.deepEqual(result.files.map((file) => [file.name, file.meshes, file.triangles, file.textureBytes]), [
    ["details.kn5", 1, 1, 20], ["main.kn5", 1, 1, 10]
  ]);
  assert.deepEqual(result.bounds.max, [100, 2, 3]);
});

test("keeps repeated workspace file placements separate", () => {
  const source = model("cone", mesh("cone", [[0, 0, 0], [1, 0, 0], [0, 1, 0]]), 8);
  const workspace = mergeKn5Models([
    { name: "cone.kn5", size: 100, model: source },
    { name: "cone.kn5", size: 100, model: source, position: [20, 0, 0] }
  ], { kind: "track", name: "layout" });
  const result = analyzeScene(workspace);
  assert.equal(result.files.length, 2);
  assert.deepEqual(result.files.map((file) => file.index).sort((a, b) => a - b), [0, 1]);
  assert.deepEqual(result.bounds.max, [21, 1, 0]);
});

test("excludes a showroom auxiliary from track totals", () => {
  const track = model("track", mesh("road", [[0, 0, 0], [1, 0, 0], [0, 1, 0]]), 8);
  const showroom = model("showroom", mesh("walls", [[0, 0, 0], [100, 0, 0], [0, 100, 0]]), 32);
  const workspace = mergeKn5Models([
    { name: "track.kn5", size: 100, model: track },
    { name: "showroom.kn5", size: 500, model: showroom, auxiliary: "reflectionEnvironment" }
  ], { kind: "track", name: "track and showroom" });
  const result = analyzeScene(workspace, { excludeAuxiliary: true });
  assert.equal(result.files.length, 1);
  assert.equal(result.files[0].name, "track.kn5");
  assert.equal(result.totals.sourceBytes, 100);
  assert.equal(result.totals.textures, 1);
  assert.equal(result.totals.effectiveTextures, 1);
  assert.equal(result.totals.textureBytes, 8);
  assert.deepEqual(result.bounds.max, [1, 1, 0]);
});

test("reports empty scenes without synthetic bounds", () => {
  const result = analyzeScene(model("empty", { kind: "node", name: "group", active: false, transform: modelPlacementMatrix(), children: [] }, 0));
  assert.equal(result.totals.meshes, 0);
  assert.equal(result.bounds, null);
  assert.equal(result.visibleBounds, null);
  assert.deepEqual(result.largestMeshes, []);
});
