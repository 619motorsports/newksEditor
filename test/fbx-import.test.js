import assert from "node:assert/strict";
import { readFile } from "node:fs/promises";
import test from "node:test";
import { BufferGeometry, Float32BufferAttribute, Group, Mesh, MeshPhongMaterial } from "three";
import { decodeDdsRgba, inspectDds } from "../src/dds.js";
import { convertFbxScene, inspectFbxHeader, parseFbx } from "../src/fbx-import.js";
import { parseKn5, walkNodes } from "../src/kn5.js";
import { serializeKn5 } from "../src/kn5-write.js";

const sdk = "/mnt/D/SteamLibrary/steamapps/common/assettocorsa/sdk";

test("recognizes binary and ASCII FBX headers", () => {
  const binary = new Uint8Array(27);
  binary.set(new TextEncoder().encode("Kaydara FBX Binary  \0\x1a\0"));
  new DataView(binary.buffer).setUint32(23, 7400, true);
  assert.deepEqual(inspectFbxHeader(binary), { format: "binary", version: 7400 });
  assert.deepEqual(inspectFbxHeader(new TextEncoder().encode("; FBX 7.4.0 project file\nFBXVersion: 7400\n")), { format: "ascii", version: 7400 });
  assert.throws(() => inspectFbxHeader(new Uint8Array(32)), /recognized FBX header/);
});

function triangleGeometry(vertexCount) {
  const geometry = new BufferGeometry(), positions = new Float32Array(vertexCount * 3), normals = new Float32Array(vertexCount * 3), uvs = new Float32Array(vertexCount * 2);
  for (let index = 0; index < vertexCount; index++) { positions[index * 3] = index % 3 === 1 ? 1 : 0; positions[index * 3 + 1] = index % 3 === 2 ? 1 : 0; normals[index * 3 + 2] = 1; }
  geometry.setAttribute("position", new Float32BufferAttribute(positions, 3)); geometry.setAttribute("normal", new Float32BufferAttribute(normals, 3)); geometry.setAttribute("uv", new Float32BufferAttribute(uvs, 2));
  return geometry;
}

test("splits material groups and meshes that exceed the KN5 index limit", () => {
  const scene = new Group(), materials = [new MeshPhongMaterial({ name: "A" }), new MeshPhongMaterial({ name: "B" })], grouped = new Mesh(triangleGeometry(6), materials);
  grouped.name = "Grouped"; grouped.geometry.addGroup(0, 3, 0); grouped.geometry.addGroup(3, 3, 1); scene.add(grouped);
  const large = new Mesh(triangleGeometry(65538), materials[0]); large.name = "Large"; scene.add(large);
  const model = convertFbxScene(scene), meshes = walkNodes(model.root).map(({ node }) => node).filter((node) => node.kind === "mesh");
  assert.deepEqual(meshes.slice(0, 2).map((mesh) => [mesh.name, mesh.materialId]), [["Grouped_SUB0", 0], ["Grouped_SUB1", 1]]);
  assert.deepEqual(meshes.slice(2).map((mesh) => mesh.vertices.length / mesh.vertexStride), [65535, 3]);
  assert.ok(meshes.every((mesh) => Math.max(...mesh.indices.subarray(Math.max(0, mesh.indices.length - 4))) <= 0xffff));
});

test("generated FBX material colors are valid one-pixel DDS textures", async (t) => {
  let bytes;
  try { bytes = await readFile(`${sdk}/editor/content/objects3D/sphere.FBX`); }
  catch { t.skip("Assetto Corsa SDK sphere fixture is not installed"); return; }
  const model = parseFbx(bytes, "sphere.FBX"), descriptor = inspectDds(model.textures[0].data), levels = decodeDdsRgba(model.textures[0].data, descriptor);
  assert.equal(descriptor.width, 1); assert.equal(descriptor.height, 1); assert.equal(levels[0].pixels.length, 4);
});

test("imports and round-trips the official SDK sphere FBX", async (t) => {
  let bytes;
  try { bytes = await readFile(`${sdk}/editor/content/objects3D/sphere.FBX`); }
  catch { t.skip("Assetto Corsa SDK sphere fixture is not installed"); return; }
  const model = parseFbx(bytes, "sphere.FBX"), meshes = walkNodes(model.root).map(({ node }) => node).filter((node) => node.kind === "mesh");
  assert.equal(model.fbx.version, 7200); assert.equal(model.fbx.format, "binary"); assert.equal(meshes.length, 1); assert.equal(meshes[0].indices.length / 3, 224);
  assert.ok(meshes[0].vertices[7] <= 0); assert.equal(meshes[0].castShadows, true);
  const reparsed = parseKn5(serializeKn5(model)), outputMesh = walkNodes(reparsed.root).map(({ node }) => node).find((node) => node.kind === "mesh");
  assert.equal(outputMesh.indices.length, meshes[0].indices.length); assert.equal(reparsed.materials[0].shader, "ksPerPixel"); assert.equal(reparsed.textures.length, 1);
});

test("imports static and skinned geometry from the official GT40 FBX", async (t) => {
  let bytes;
  try { bytes = await readFile(`${sdk}/dev/car_pipeline_2.0rev/Scene templates/GT40_animated_suspension_example_fbx.FBX`); }
  catch { t.skip("Assetto Corsa SDK GT40 fixture is not installed"); return; }
  const model = parseFbx(bytes, "GT40_animated_suspension_example_fbx.FBX"), meshes = walkNodes(model.root).map(({ node }) => node).filter((node) => node.kind === "mesh" || node.kind === "skinnedMesh");
  assert.equal(model.fbx.version, 7300); assert.equal(model.fbx.animations.length, 1); assert.equal(meshes.length, 42); assert.equal(meshes.filter((node) => node.kind === "skinnedMesh").length, 4);
  assert.equal(meshes.reduce((sum, node) => sum + node.indices.length / 3, 0), 16514);
  const reparsed = parseKn5(serializeKn5(model)), reparsedMeshes = walkNodes(reparsed.root).map(({ node }) => node).filter((node) => node.kind === "mesh" || node.kind === "skinnedMesh");
  assert.equal(reparsedMeshes.length, 42); assert.equal(reparsedMeshes.filter((node) => node.kind === "skinnedMesh").length, 4);
});
