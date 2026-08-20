import assert from "node:assert/strict";
import { readFile } from "node:fs/promises";
import { join } from "node:path";
import test from "node:test";
import { computeKn5Visibility, Kn5Error, parseKn5, walkNodes } from "../src/kn5.js";

const fixture = "/mnt/D/SteamLibrary/SteamLibrary/steamapps/common/assettocorsa/content/cars/ks_nissan_370z/collider.kn5";
const carFixture = "/mnt/D/SteamLibrary/SteamLibrary/steamapps/common/assettocorsa/content/cars/ks_nissan_370z/nissan_370z.kn5";
const trackFixture = "/mnt/D/SteamLibrary/SteamLibrary/steamapps/common/assettocorsa/content/tracks/imola/2.kn5";
const v5Fixture = "/mnt/D/SteamLibrary/SteamLibrary/steamapps/common/assettocorsa/content/cars/abarth500/collider.kn5";
const protectedV5Fixture = "/mnt/D/SteamLibrary/SteamLibrary/steamapps/common/assettocorsa/content/cars/ac_friends_488_gte_imsa/ferrari_488_gte.kn5";
const resourceSlotFixture = join(
  process.env.ASSETTO_CORSA_ROOT || "/mnt/D/SteamLibrary/steamapps/common/assettocorsa",
  "content", "cars", "bmw_m3_e92", "bmw_m3_e92.kn5"
);

test("parses a real Kunos collider KN5", async (t) => {
  let data;
  try { data = await readFile(fixture); } catch { t.skip("Assetto Corsa fixture is not installed"); return; }
  const model = parseKn5(data);
  assert.equal(model.version, 6);
  assert.equal(model.materials.length, 1);
  assert.equal(model.materials[0].name, "GL");
  assert.equal(model.root.name, "FBX: collider.FBX");
  const nodes = walkNodes(model.root);
  assert.equal(nodes.length, 3);
  assert.equal(nodes[2].node.kind, "mesh");
  assert.equal(nodes[2].node.vertices.length, 28 * 11);
  assert.equal(nodes[2].node.indices.length, 156);
  assert.equal(model.bytesRead, model.byteLength);
});

test("parses the KN5 v5 header without a v6 source marker", async (t) => {
  let data;
  try { data = await readFile(v5Fixture); } catch { t.skip("Assetto Corsa v5 fixture is not installed"); return; }
  const model = parseKn5(data);
  assert.equal(model.version, 5);
  assert.equal(model.source, 0);
  assert.equal(model.textures.length, 0);
  assert.equal(model.materials[0].name, "CAR_Cerchione");
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

test("consumes a complete textured car KN5 with skinned nodes", async (t) => {
  let data;
  try { data = await readFile(carFixture); } catch { t.skip("Assetto Corsa car fixture is not installed"); return; }
  const model = parseKn5(data);
  const nodes = walkNodes(model.root);
  assert.ok(model.textures.length > 0);
  assert.ok(model.materials.length > 0);
  assert.ok(nodes.some(({ node }) => node.kind === "mesh"));
  assert.ok(nodes.some(({ node }) => node.kind === "skinnedMesh"));
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

test("consumes a complete track KN5", async (t) => {
  let data;
  try { data = await readFile(trackFixture); } catch { t.skip("Assetto Corsa track fixture is not installed"); return; }
  const model = parseKn5(data);
  assert.ok(walkNodes(model.root).filter(({ node }) => node.kind === "mesh").length >= 2);
  assert.equal(model.bytesRead, model.byteLength);
});

test("rejects invalid and truncated files with offsets", () => {
  assert.throws(() => parseKn5(new TextEncoder().encode("not-kn5")), Kn5Error);
  const header = new Uint8Array([115, 99, 54, 57, 54, 57, 6, 0, 0, 0]);
  assert.throws(() => parseKn5(header), /source marker.*0xa/);
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
