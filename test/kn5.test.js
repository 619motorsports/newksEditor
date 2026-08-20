import assert from "node:assert/strict";
import { readFile } from "node:fs/promises";
import test from "node:test";
import { computeKn5Visibility, Kn5Error, parseKn5, walkNodes } from "../src/kn5.js";
import { assettoPath, carColliderKn5, carMainKn5, trackMainKn5 } from "./fixture-paths.js";

const protectedV5Fixture = assettoPath("content/cars/ac_friends_488_gte_imsa/ferrari_488_gte.kn5");
const editorAxisV4Fixture = assettoPath("sdk/editor/content/objects3D/axis.kn5");
const resourceSlotFixture = assettoPath("content/cars/bmw_m3_e92/bmw_m3_e92.kn5");

test("parses the repository car collider KN5", async () => {
  const data = await readFile(carColliderKn5);
  const model = parseKn5(data);
  assert.equal(model.version, 6);
  assert.equal(model.materials.length, 1);
  assert.equal(model.materials[0].name, "UPG0");
  assert.equal(model.root.name, "FBX: collider.fbx");
  const nodes = walkNodes(model.root);
  assert.equal(nodes.length, 3);
  assert.equal(nodes[2].node.kind, "mesh");
  assert.equal(nodes[2].node.vertices.length, 32 * 11);
  assert.equal(nodes[2].node.indices.length, 180);
  assert.equal(model.bytesRead, model.byteLength);
});

test("parses the repository track's KN5 v5 header without a v6 source marker", async () => {
  const data = await readFile(trackMainKn5);
  const model = parseKn5(data);
  assert.equal(model.version, 5);
  assert.equal(model.source, 0);
  assert.equal(model.textures.length, 130);
  assert.equal(model.materials.length, 167);
  assert.equal(model.bytesRead, model.byteLength);
});

test("consumes the installed editor axis KN5 v4 without a material depth field", async (t) => {
  let data;
  try { data = await readFile(editorAxisV4Fixture); } catch { t.skip("Assetto Corsa editor fixture is not installed"); return; }
  const model = parseKn5(data);
  assert.equal(model.version, 4);
  assert.equal(model.source, 0);
  assert.equal(model.textures.length, 1);
  assert.equal(model.materials.length, 1);
  assert.equal(model.materials[0].depthMode, 0);
  assert.equal(model.root.name, "FBX: axis.fbx");
  const nodes = walkNodes(model.root);
  assert.equal(nodes.length, 3);
  assert.equal(nodes[2].node.kind, "mesh");
  assert.equal(nodes[2].node.vertices.length, 478 * 11);
  assert.equal(nodes[2].node.indices.length, 1734);
  assert.equal(model.bytesRead, model.byteLength);
});

test("recognizes a CSP KN5ENC v1 payload after a complete public v5 scene", async (t) => {
  let data;
  try { data = await readFile(protectedV5Fixture); } catch { t.skip("CSP-protected fixture is not installed"); return; }
  const model = parseKn5(data);
  assert.equal(model.version, 5);
  assert.equal(model.materials.length, 70);
  assert.equal(model.encryption?.format, "CSP_KN5ENC_v1");
  assert.equal(model.encryption?.valid, true);
  assert.equal(model.encryption?.recordCount, 1002);
  assert.ok(model.encryption?.protectedTextures.includes("skin_base.dds"));
  assert.ok(model.encryption?.protectedMeshes.includes("GEO_Mini_Lights_GLASS"));
  assert.equal(model.encryption.payloadOffset, model.bytesRead);
});

test("consumes the complete repository car KN5", async () => {
  const data = await readFile(carMainKn5);
  const model = parseKn5(data);
  const nodes = walkNodes(model.root);
  assert.ok(model.textures.length > 0);
  assert.ok(model.materials.length > 0);
  assert.ok(nodes.some(({ node }) => node.kind === "mesh"));
  assert.equal(nodes.length, 281);
  assert.equal(model.bytesRead, model.byteLength);
});

test("reads native shader resource slots independently of texture indices", async (t) => {
  let data;
  try { data = await readFile(resourceSlotFixture); } catch { t.skip("The Kunos BMW M3 E92 fixture is not installed"); return; }
  const model = parseKn5(data, { metadataOnly: true }), textureIndices = new Map(model.textures.map((texture, index) => [texture.name.toLowerCase(), index]));
  const lights = model.materials.find((material) => material.name === "CAR_lights"), chassis = model.materials.find((material) => material.name === "CAR_chassis");

  assert.deepEqual(lights.resources.map((resource) => resource.textureId), [0, 1, 2, 3]);
  assert.deepEqual(lights.resources.map((resource) => textureIndices.get(resource.texture.toLowerCase())), [5, 6, 7, 5]);
  assert.deepEqual(chassis.resources[6], { slot: "txDamageMask", textureId: 21, texture: "damage_mask.dds" });
  assert.equal(textureIndices.get(chassis.resources[6].texture), 18);
});

test("consumes the complete repository track KN5", async () => {
  const data = await readFile(trackMainKn5);
  const model = parseKn5(data);
  assert.equal(walkNodes(model.root).filter(({ node }) => node.kind === "mesh").length, 1070);
  assert.equal(model.bytesRead, model.byteLength);
});

test("rejects invalid and truncated files with offsets", () => {
  assert.throws(() => parseKn5(new TextEncoder().encode("not-kn5")), Kn5Error);
  const header = new Uint8Array([115, 99, 54, 57, 54, 57, 6, 0, 0, 0]);
  assert.throws(() => parseKn5(header), /source marker.*0xa/);
  const truncatedV4 = new Uint8Array([
    115, 99, 54, 57, 54, 57,
    4, 0, 0, 0,
    0, 0, 0, 0,
    1, 0, 0, 0,
    100, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
  ]);
  assert.throws(() => parseKn5(truncatedV4), /material name.*0x16/);
});

test("computes game visibility from active branches and mesh flags", () => {
  const visible = { kind: "mesh", name: "visible", active: true, visible: true, renderable: true, children: [] };
  const collision = { kind: "mesh", name: "collision", active: true, visible: true, renderable: false, children: [] };
  const child = { kind: "mesh", name: "inactive child", active: true, visible: true, renderable: true, children: [] };
  const inactive = { kind: "node", name: "inactive", active: false, children: [child] };
  const root = { kind: "node", name: "root", active: true, children: [visible, collision, inactive] };
  const state = computeKn5Visibility(root);
  assert.equal(state.get(root), true);
  assert.equal(state.get(visible), true);
  assert.equal(state.get(collision), false);
  assert.equal(state.get(inactive), false);
  assert.equal(state.get(child), false);
});
