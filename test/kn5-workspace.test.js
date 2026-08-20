import assert from "node:assert/strict";
import { readdir, readFile } from "node:fs/promises";
import test from "node:test";
import { findAcdEntry, parseAcd } from "../src/acd.js";
import { carLodDistance, carLodVisible, mergeKn5Models, modelPlacementMatrix, normalizeCarLodFileName, parseCarLodsIni, parseModelsIni, serializeCarLodsIni, serializeModelsIni } from "../src/kn5-workspace.js";
import { parseKn5, walkNodes } from "../src/kn5.js";
import { assettoPath, carFixtureRoot } from "./fixture-paths.js";

const hickoryFixture = assettoPath("content/tracks/hickory");

function model(name, materialName, textureName, materialId = 0) {
  return {
    magic: "sc6969", version: 6, source: 1, bytesRead: 100, byteLength: 100,
    textures: [{ name: textureName, size: name.length, data: new Uint8Array() }],
    materials: [{ name: materialName, shader: "ksPerPixel", properties: [], resources: [] }],
    root: { kind: "node", name, active: true, transform: modelPlacementMatrix(), children: [{ kind: "mesh", name: `${name}_mesh`, active: true, materialId, children: [] }] }
  };
}

test("parses ordered static and dynamic layout objects", () => {
  const parsed = parseModelsIni(`
[MODEL_2]
FILE='details.kn5'
POSITION=1, 2, 3
ROTATION=90, 0, 0
[DYNAMIC_OBJECT_0]
FILE=plane.kn5
PROBABILITY=75
MULT=1,3
RND_POS_CENTER=100,200,300
RND_POS_RANGE=10,20,30
RND_VEL_BASE=1,2,3
RND_VEL_RANGE=4,5,6
[MODEL_0]
FILE=main.kn5
`);
  assert.deepEqual(parsed.models.map((entry) => entry.file), ["main.kn5", "details.kn5"]);
  assert.deepEqual(parsed.models[1].position, [1, 2, 3]);
  assert.deepEqual(parsed.models[1].rotation, [90, 0, 0]);
  assert.equal(parsed.dynamicObjects[0].file,"plane.kn5");
  assert.equal(parsed.dynamicObjects[0].probability,75);
  assert.deepEqual(parsed.dynamicObjects[0].multiplicity,[1,3]);
  assert.deepEqual(parsed.dynamicObjects[0].positionCenter,[100,200,300]);
  assert.deepEqual(parsed.dynamicObjects[0].velocityRange,[4,5,6]);
  assert.equal(parsed.ignoredSections, 0);
});

test("serializes track manifests without losing static or dynamic fields", () => {
  const workspace = { files: [
    { name: "main.kn5", manifestIndex: 0, position: [0, 1.5, 0], rotation: [0, 90, 0] },
    { name: "plane.kn5", position: [-10, 20, 30], dynamic: { index: 2, probability: 75, multiplicity: [1, 3], posMode: "RANDOM", positionCenter: [-10, 20, 30], positionRange: [4, 5, 6], velMode: "RANDOM", velocityBase: [1, 2, 3], velocityRange: [7, 8, 9], playWav: "fly by.wav" } }
  ] };
  const text = serializeModelsIni(workspace), parsed = parseModelsIni(text);
  assert.deepEqual(parsed.models[0], { index: 0, file: "main.kn5", position: [0, 1.5, 0], rotation: [0, 90, 0], section: "MODEL_0", line: 1 });
  assert.equal(parsed.dynamicObjects[0].index, 2);
  assert.equal(parsed.dynamicObjects[0].probability, 75);
  assert.deepEqual(parsed.dynamicObjects[0].positionRange, [4, 5, 6]);
  assert.deepEqual(parsed.dynamicObjects[0].velocityRange, [7, 8, 9]);
  assert.equal(parsed.dynamicObjects[0].playWav, "fly by.wav");
  assert.deepEqual(parsed.warnings, []);
});

test("assigns independent fallback indices to manual static and dynamic track entries", () => {
  const parsed = parseModelsIni(serializeModelsIni({ files: [
    { name: "main.kn5", manifestIndex: 1, position: [0, 0, 0], rotation: [0, 0, 0] },
    { name: "balloon.kn5", dynamic: { index: 1, probability: 100 } },
    { name: "details.kn5", position: [0, 0, 0], rotation: [0, 0, 0] },
    { name: "plane.kn5", dynamic: { probability: 50 } }
  ] }));
  assert.deepEqual(parsed.models.map((entry) => [entry.index, entry.file]), [[0, "details.kn5"], [1, "main.kn5"]]);
  assert.deepEqual(parsed.dynamicObjects.map((entry) => [entry.index, entry.file]), [[0, "plane.kn5"], [1, "balloon.kn5"]]);
});

test("quotes manifest file names without removing apostrophes", () => {
  const parsed = parseModelsIni(serializeModelsIni({ files: [
    { name: "O'Brien main.kn5", position: [0, 0, 0], rotation: [0, 0, 0] }
  ] }));
  assert.equal(parsed.models[0].file, "O'Brien main.kn5");
  assert.throws(() => serializeModelsIni({ files: [
    { name: `both ' and " quotes.kn5`, position: [0, 0, 0], rotation: [0, 0, 0] }
  ] }), /both quote characters/);
});

test("parses installed Kunos dynamic track objects",async(t)=>{let text;try{text=await readFile(assettoPath("content/tracks/ks_barcelona/models_layout_gp.ini"),"utf8");}catch{t.skip("Assetto Corsa dynamic track fixture is not installed");return;}const parsed=parseModelsIni(text,"models_layout_gp.ini");assert.equal(parsed.models.length,9);assert.equal(parsed.dynamicObjects.length,2);assert.deepEqual(parsed.dynamicObjects[0].positionCenter,[-320,160,1400]);assert.deepEqual(parsed.dynamicObjects[0].velocityRange,[2,0,2]);assert.equal(parsed.dynamicObjects[0].probability,75);});

test("builds heading, pitch, roll placement matrices with translation", () => {
  const matrix = modelPlacementMatrix([10, 20, 30], [90, 0, 0]);
  assert.deepEqual(matrix.slice(12, 15), [10, 20, 30]);
  assert.ok(Math.abs(matrix[0]) < 1e-10);
  assert.equal(matrix[2], 1);
  assert.equal(matrix[8], -1);
  const combined = modelPlacementMatrix([0, 0, 0], [90, 90, 0]).map((value) => Math.abs(value) < 1e-10 ? 0 : value);
  assert.deepEqual(combined.slice(0, 12), [0, 1, 0, 0, 0, 0, -1, 0, -1, 0, 0, 0]);
});

test("parses the contiguous car LOD sequence and distance switches", () => {
  const parsed = parseCarLodsIni(`
[COCKPIT_HR]
DISTANCE_SWITCH=6
[DRIVER_HR]
DISTANCE_SWITCH=25
[LOD_0]
FILE='car.kn5'
IN=0
OUT=15
[LOD_1]
FILE=car_lod_b.kn5
IN=15
OUT=45
[LOD_3]
FILE=ignored.kn5
IN=200
OUT=5000
`);
  assert.deepEqual(parsed.lods.map((entry) => entry.file), ["car.kn5", "car_lod_b.kn5"]);
  assert.deepEqual(parsed.lods.map((entry) => [entry.in, entry.out]), [[0, 15], [15, 45]]);
  assert.equal(parsed.cockpitHrDistance, 6);
  assert.equal(parsed.driverHrDistance, 25);
  assert.ok(parsed.warnings.some((warning) => /LOD_3 is ignored.*missing LOD_2/.test(warning)));
});

test("serializes contiguous car LOD ranges and distance switches", () => {
  const workspace = { cockpitHrDistance: 6, driverHrDistance: 25, files: [
    { name: "car.kn5", lod: { index: 0, in: 0, out: 15 } },
    { name: "car_lod_b.kn5", lod: { index: 1, in: 15, out: 45 } },
    { name: "Driver.kn5", auxiliary: "driver" }
  ] };
  const text = serializeCarLodsIni(workspace), parsed = parseCarLodsIni(text);
  assert.deepEqual(parsed.lods.map((lod) => [lod.file, lod.in, lod.out]), [["car.kn5", 0, 15], ["car_lod_b.kn5", 15, 45]]);
  assert.equal(parsed.cockpitHrDistance, 6);
  assert.equal(parsed.driverHrDistance, 25);
  assert.deepEqual(parsed.warnings, []);
});

test("normalizes portable car LOD file names without rejecting game-used relative paths", () => {
  assert.equal(normalizeCarLodFileName(" NASCAR Craftsman Truck Series.kn5 "), "NASCAR Craftsman Truck Series.kn5");
  assert.equal(normalizeCarLodFileName("..\\shared_car\\body_lod_b.kn5"), "../shared_car/body_lod_b.kn5");
  for (const value of ["", "/car.kn5", "C:\\cars\\car.kn5", "car.fbx", "folder//car.kn5", "folder/./car.kn5", "CON.kn5", "car?.kn5", "car.kn5\n[LOD_1]"]) {
    assert.throws(() => normalizeCarLodFileName(value), /car LOD file name/i);
  }
});

test("quotes authored car LOD file names and rejects malformed output", () => {
  const workspace = { files: [{ name: "Car LOD A.kn5", lod: { index: 0, in: 0, out: 50 } }] };
  const text = serializeCarLodsIni(workspace);
  assert.match(text, /FILE='Car LOD A\.kn5'/);
  assert.equal(parseCarLodsIni(text).lods[0].file, "Car LOD A.kn5");
  workspace.files[0].name = "../shared/car_lod.kn5";
  assert.match(serializeCarLodsIni(workspace), /FILE=\.\.\/shared\/car_lod\.kn5/);
  workspace.files[0].name = "car?.kn5";
  assert.throws(() => serializeCarLodsIni(workspace), /not portable/);
});

test("accepts every installed car LOD file name", async (t) => {
  const root = assettoPath("content/cars");
  let names;
  try { names = await readdir(root); }
  catch { t.skip("Installed Assetto Corsa car archives are unavailable"); return; }
  let archives = 0, entries = 0;
  for (const name of names) {
    let archive;
    try { archive = parseAcd(await readFile(`${root}/${name}/data.acd`), name); }
    catch { continue; }
    const entry = findAcdEntry(archive, "lods.ini");
    if (!entry) continue;
    archives++;
    for (const lod of parseCarLodsIni(new TextDecoder().decode(entry.data), `${name}/lods.ini`).lods) {
      assert.doesNotThrow(() => normalizeCarLodFileName(lod.file), `${name}: ${lod.file}`);
      entries++;
    }
  }
  assert.ok(archives > 0 && entries > 0);
});

test("diagnoses car LOD gaps and overlaps and uses half-open preview ranges", () => {
  const gap = parseCarLodsIni("[LOD_0]\nFILE=a.kn5\nIN=0\nOUT=10\n[LOD_1]\nFILE=b.kn5\nIN=12\nOUT=20");
  const overlap = parseCarLodsIni("[LOD_0]\nFILE=a.kn5\nIN=0\nOUT=15\n[LOD_1]\nFILE=b.kn5\nIN=10\nOUT=20");
  assert.ok(gap.warnings.some((warning) => /2 m gap/.test(warning)));
  assert.ok(overlap.warnings.some((warning) => /overlap by 5 m/.test(warning)));
  assert.equal(carLodVisible(gap.lods[0], 9.999), true);
  assert.equal(carLodVisible(gap.lods[0], 10), false);
  assert.equal(carLodVisible(gap.lods[1], 12), true);
  assert.equal(carLodVisible(gap.lods[1], 1, 1), true);
  assert.equal(carLodVisible(gap.lods[0], 1, 1), false);
  assert.equal(carLodDistance(20, 45), 15);
  assert.equal(carLodDistance(20, 60, 2), 10);
  assert.equal(carLodDistance(20, 60, 1, true), 2);
});

test("merges KN5 scenes and remaps material IDs without mutating inputs", () => {
  const first = model("first", "road", "shared.dds"), second = model("second", "grass", "shared.dds");
  const merged = mergeKn5Models([{ name: "first.kn5", model: first }, { name: "second.kn5", model: second, position: [4, 5, 6] }], { name: "layout", manifest: "models.ini" });
  assert.equal(merged.materials.length, 2);
  assert.equal(merged.textures.length, 1);
  assert.equal(merged.workspace.textureCollisions.length, 1);
  assert.equal(merged.root.children[0].children[0].children[0].materialId, 0);
  assert.equal(merged.root.children[1].children[0].children[0].materialId, 1);
  assert.equal(second.root.children[0].materialId, 0);
  assert.deepEqual(merged.root.children[1].transform.slice(12, 15), [4, 5, 6]);
  assert.equal(merged.bytesRead, 200);
});

test("preserves track manifest indices in merged workspace metadata", () => {
  const merged = mergeKn5Models([
    { name: "main.kn5", model: model("main", "road", "road.dds"), manifestIndex: 0 },
    { name: "details.kn5", model: model("details", "signs", "signs.dds"), manifestIndex: 3 }
  ]);
  assert.deepEqual(merged.workspace.files.map((file) => file.manifestIndex), [0, 3]);
  assert.deepEqual(parseModelsIni(serializeModelsIni(merged.workspace)).models.map((entry) => entry.index), [0, 3]);
});

test("preserves car LOD ranges on merged workspace roots", () => {
  const merged = mergeKn5Models([
    { name: "car.kn5", model: model("a", "body", "shared.dds"), lod: { index: 0, in: 0, out: 15 } },
    { name: "car_lod_b.kn5", model: model("longer", "body_lod", "shared.dds"), lod: { index: 1, in: 15, out: 45 } }
  ], { name: "car", kind: "carLods", manifest: "data/lods.ini", packedManifest: true, cockpitHrDistance: 6, driverHrDistance: 25 });
  assert.equal(merged.workspace.kind, "carLods");
  assert.equal(merged.workspace.packedManifest, true);
  assert.deepEqual(merged.root.children.map((node) => node.workspaceLod), [{ index: 0, in: 0, out: 15 }, { index: 1, in: 15, out: 45 }]);
  assert.deepEqual(merged.workspace.files.map((file) => file.lod), [{ index: 0, in: 0, out: 15 }, { index: 1, in: 15, out: 45 }]);
  assert.equal(merged.workspace.cockpitHrDistance, 6);
  assert.equal(merged.textures.length, 2);
  assert.deepEqual(merged.textures.map((texture) => texture.workspaceFile), ["car.kn5", "car_lod_b.kn5"]);
  assert.deepEqual(merged.materials.map((material) => material.workspaceFile), ["car.kn5", "car_lod_b.kn5"]);
  assert.equal(merged.workspace.textureCollisions[0].sizesDiffer, true);
});

test("scopes car and reflection-environment resources without name collisions", () => {
  const merged = mergeKn5Models([
    { name: "car.kn5", model: model("car", "body", "shared.dds") },
    { name: "hangar.kn5", model: model("hangar", "floor", "shared.dds"), auxiliary: "reflectionEnvironment" }
  ], { name: "car + hangar", kind: "carEnvironment" });
  assert.equal(merged.workspace.scopeResources, true);
  assert.deepEqual(merged.materials.map((material) => material.workspaceFile), ["car.kn5", "hangar.kn5"]);
  assert.deepEqual(merged.textures.map((texture) => texture.workspaceFile), ["car.kn5", "hangar.kn5"]);
  assert.equal(merged.root.children[1].workspaceAuxiliary, "reflectionEnvironment");
});

test("rejects empty or malformed workspaces", () => {
  assert.throws(() => mergeKn5Models([]), /at least one model/);
  assert.throws(() => mergeKn5Models([{ name: "broken.kn5", model: {} }]), /not a parsed KN5/);
});

test("assembles a complete installed multi-KN5 track manifest", async (t) => {
  let manifestText;
  try { manifestText = await readFile(`${hickoryFixture}/models.ini`, "utf8"); }
  catch { t.skip("Assetto Corsa multi-KN5 fixture is not installed"); return; }
  const manifest = parseModelsIni(manifestText, "models.ini"), entries = [];
  for (const item of manifest.models) {
    const data = await readFile(`${hickoryFixture}/${item.file}`), parsed = parseKn5(data);
    entries.push({ name: item.file, size: data.byteLength, model: parsed, position: item.position, rotation: item.rotation });
  }
  const merged = mergeKn5Models(entries, { name: "hickory", manifest: "models.ini" });
  assert.equal(merged.workspace.files.length, 2);
  assert.equal(merged.materials.length, entries.reduce((sum, entry) => sum + entry.model.materials.length, 0));
  assert.equal(walkNodes(merged.root).length, 1 + entries.length + entries.reduce((sum, entry) => sum + walkNodes(entry.model.root).length, 0));
  assert.ok(walkNodes(merged.root).filter(({ node }) => node.kind === "mesh" || node.kind === "skinnedMesh").every(({ node }) => node.materialId >= 0 && node.materialId < merged.materials.length));
  assert.equal(merged.bytesRead, merged.byteLength);
});

test("assembles the complete repository four-level car LOD manifest", async () => {
  const manifestText = await readFile(`${carFixtureRoot}/data/lods.ini`, "utf8");
  const manifest = parseCarLodsIni(manifestText, "data/lods.ini"), entries = [];
  for (const lod of manifest.lods) {
    const data = await readFile(`${carFixtureRoot}/${lod.file}`), parsed = parseKn5(data);
    entries.push({ name: lod.file, size: data.byteLength, model: parsed, lod });
  }
  const merged = mergeKn5Models(entries, { name: "619_gen6_arca_base", kind: "carLods", manifest: "data/lods.ini", warnings: manifest.warnings, cockpitHrDistance: manifest.cockpitHrDistance });
  assert.equal(merged.workspace.files.length, 4);
  assert.deepEqual(merged.workspace.files.map((file) => [file.lod.in, file.lod.out]), [[0, 15], [15, 45], [45, 201], [201, 2000]]);
  assert.equal(merged.workspace.cockpitHrDistance, 25);
  assert.equal(merged.workspace.warnings.length, 0);
  assert.equal(merged.bytesRead, merged.byteLength);
  assert.ok(walkNodes(merged.root).filter(({ node }) => node.kind === "mesh" || node.kind === "skinnedMesh").every(({ node }) => node.materialId >= 0 && node.materialId < merged.materials.length));
});
