import assert from "node:assert/strict";
import test from "node:test";
import { classifyEditorProjectChanges, cloneEditorProject, createEditorProject, editorProjectCspEditCount, editorProjectEditCount, editorProjectKn5EditCount, formatEditorValue, normalizeEditorProject, parseEditorValue, serializeEditorCsp, serializeEditorProject } from "../src/editor-project.js";
import { evaluateCspConfig, parseCspIni } from "../src/csp-config.js";

test("normalizes portable projects and rejects incompatible input", () => {
  const project = normalizeEditorProject({ format: "apex-editor-project", version: 1, asset: { name: "car.kn5", size: 42, kn5Version: 6 }, materialEdits: { body: { shader: "smCarPaint", properties: { ksDiffuse: 0.5, ksEmissive: [1, 0, 0] }, resources: { txMaps: { color: [1, 1, 1, 1] } } } }, meshEdits: { BODY: { isTransparent: false, layer: 3 } }, nodeEdits: { "0": { name: "BODY_RENAMED", active: false, transform: [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 2, 3, 4, 1] } }, geometryEdits: { "0/0": { transform: [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, -1, 2, 3, 1], removeDegenerate: true, recalculateNormals: true } }, workspaceEdits: { files: { "1": { lodIn: 15, lodOut: 45 } }, cockpitHrDistance: 6 } });
  assert.equal(project.asset.name, "car.kn5");
  assert.equal(editorProjectEditCount(project), 15);
  assert.equal(editorProjectKn5EditCount(project), 12);
  assert.equal(editorProjectCspEditCount(project), 6);
  assert.equal(project.nodeEdits["0"].name, "BODY_RENAMED");
  assert.equal(project.nodeEdits["0"].active, false);
  assert.deepEqual(project.nodeEdits["0"].transform.slice(12, 15), [2, 3, 4]);
  assert.deepEqual(project.geometryEdits["0/0"].transform.slice(12, 15), [-1, 2, 3]);
  assert.equal(project.geometryEdits["0/0"].removeDegenerate, true);
  assert.equal(project.geometryEdits["0/0"].recalculateNormals, true);
  assert.deepEqual(project.workspaceEdits.files["1"], { lodIn: 15, lodOut: 45 });
  assert.equal(project.workspaceEdits.cockpitHrDistance, 6);
  assert.throws(() => normalizeEditorProject({ format: "other", version: 1 }), /Not an Apex Editor project/);
  assert.throws(() => normalizeEditorProject({ format: "apex-editor-project", version: 99 }), /Unsupported/);
});

test("normalizes dynamic track manifest edits as non-CSP project fields", () => {
  const project = normalizeEditorProject({ format: "apex-editor-project", version: 1, workspaceEdits: { files: { "2": { probability: 25, multiplicity: [2, 5], posMode: "fixed", positionCenter: [-10, 40, 50], positionRange: [1, 2, 3], velMode: "linear", velocityBase: [8, 9, 10], velocityRange: [0, 1, 2], playWav: null } } } });
  assert.deepEqual(project.workspaceEdits.files["2"], { probability: 25, multiplicity: [2, 5], posMode: "FIXED", positionCenter: [-10, 40, 50], positionRange: [1, 2, 3], velMode: "LINEAR", velocityBase: [8, 9, 10], velocityRange: [0, 1, 2], playWav: null });
  assert.equal(editorProjectEditCount(project), 9);
  assert.equal(editorProjectCspEditCount(project), 0);
});

test("normalizes portable car LOD file-name edits and drops malformed names", () => {
  const project = normalizeEditorProject({ format: "apex-editor-project", version: 1, workspaceEdits: { files: {
    "0": { name: "..\\shared_car\\body lod.kn5" },
    "1": { name: "C:\\cars\\body.kn5" },
    "2": { name: "body.fbx" }
  } } });
  assert.deepEqual(project.workspaceEdits.files["0"], { name: "../shared_car/body lod.kn5" });
  assert.equal(project.workspaceEdits.files["1"], undefined);
  assert.equal(project.workspaceEdits.files["2"], undefined);
  assert.equal(editorProjectEditCount(project), 1);
  assert.equal(editorProjectCspEditCount(project), 0);
});

test("normalizes track surface edits as non-CSP project fields", () => {
  const project = normalizeEditorProject({ format: "apex-editor-project", version: 1, surfaceEdits: { "0": { key: "tarmac", friction: "1.05", damping: 0.02, dirtAdditive: 0.1, blackFlagTime: 3, isValidTrack: true, isPitlane: false, sinHeight: 0.001, sinLength: 2.5, vibrationGain: 0.15, vibrationLength: 0.4, wav: null, wavPitch: 1.2, ffEffect: "GRAIN" } } });
  assert.deepEqual(project.surfaceEdits["0"], { key: "TARMAC", friction: 1.05, damping: 0.02, dirtAdditive: 0.1, blackFlagTime: 3, isValidTrack: true, isPitlane: false, sinHeight: 0.001, sinLength: 2.5, vibrationGain: 0.15, vibrationLength: 0.4, wav: null, wavPitch: 1.2, ffEffect: "GRAIN" });
  assert.equal(editorProjectEditCount(project), 14);
  assert.equal(editorProjectCspEditCount(project), 0);
});

test("normalizes skin metadata edits as non-CSP project fields", () => {
  const project = normalizeEditorProject({ format: "apex-editor-project", version: 1, skinEdits: {
    red: { skinname: "Rosso", drivername: "Driver", country: "Italy", team: "Works", number: 7, priority: 30, ignored: true },
    blue: { skinname: "Blue", priority: -1 }
  } });
  assert.deepEqual({ ...project.skinEdits.red }, { skinname: "Rosso", drivername: "Driver", country: "Italy", team: "Works", priority: 30 });
  assert.deepEqual({ ...project.skinEdits.blue }, { skinname: "Blue" });
  assert.equal(editorProjectEditCount(project), 6);
  assert.equal(editorProjectKn5EditCount(project), 0);
  assert.equal(editorProjectCspEditCount(project), 0);
  assert.equal(classifyEditorProjectChanges(createEditorProject(), project).skinChanged, true);
  assert.deepEqual(JSON.parse(serializeEditorProject(project)).skinEdits.red, { skinname: "Rosso", drivername: "Driver", country: "Italy", team: "Works", priority: 30 });
});

test("classifies surface edits without marking scene geometry as changed", () => {
  const before = createEditorProject({ name: "track.kn5", size: 42, kn5Version: 6 }), surface = cloneEditorProject(before);
  surface.surfaceEdits["0"] = { friction: 1.05 };
  assert.deepEqual(classifyEditorProjectChanges(before, surface), { geometryChanged: false, nodeChanged: false, workspaceChanged: false, surfaceChanged: true, skinChanged: false, colliderChanged: false, damageChanged: false, sceneChanged: false });
  const node = cloneEditorProject(surface); node.nodeEdits["0"] = { active: false };
  assert.deepEqual(classifyEditorProjectChanges(surface, node), { geometryChanged: false, nodeChanged: true, workspaceChanged: false, surfaceChanged: false, skinChanged: false, colliderChanged: false, damageChanged: false, sceneChanged: true });
});

test("normalizes collider geometry edits as non-CSP project fields", () => {
  const transform = [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0.1, -0.2, 0.3, 1];
  const colliderAsset = { name: "collider.kn5", size: 1024, sha256: "ab".repeat(32), kn5Version: 6 };
  const project = normalizeEditorProject({ format: "apex-editor-project", version: 1, colliderAsset, colliderEdits: { "0": { transform, removeDegenerate: true, reverseWinding: false, recalculateNormals: true }, "1": { transform: [1, 2, 3], removeDegenerate: "yes" } } });
  assert.deepEqual(project.colliderAsset, colliderAsset);
  assert.deepEqual(project.colliderEdits["0"], { transform, removeDegenerate: true, recalculateNormals: true });
  assert.equal(project.colliderEdits["1"], undefined);
  assert.equal(editorProjectEditCount(project), 3);
  assert.equal(editorProjectCspEditCount(project), 0);
  assert.doesNotMatch(serializeEditorCsp(project), /collider/i);
});

test("drops malformed collider identity without applying it to old projects", () => {
  const project = normalizeEditorProject({ format: "apex-editor-project", version: 1, colliderAsset: { name: "collider.kn5", size: 10, sha256: "truncated" }, colliderEdits: { "0": { removeDegenerate: true } } });
  assert.equal(project.colliderAsset, null);
  assert.deepEqual(project.colliderEdits["0"], { removeDegenerate: true });
  assert.equal(createEditorProject().colliderAsset, null);
  const orphan = normalizeEditorProject({ format: "apex-editor-project", version: 1, colliderAsset: { name: "collider.kn5", size: 10, sha256: "ab".repeat(32) } });
  assert.equal(orphan.colliderAsset, null);
});

test("normalizes car damage edits as separate non-CSP project fields", () => {
  const damageAsset = { name: "data/damage.ini", size: 500, sha256: "cd".repeat(32) };
  const project = normalizeEditorProject({ format: "apex-editor-project", version: 1, damageAsset, damageEdits: {
    SCRATCHES: { minSpeed: 0, maxSpeed: 30, enabled: true },
    DAMAGE: { initialLevel: 40 },
    VISUAL_OBJECT_0: { name: "HOOD_DAMAGE", damageZone: "front", staticRotationAxis: [1, 0, 0], fullSpeed: 90 },
    VISUAL_OBJECT_1: { name: "bad;name", damageZone: "bad zone", staticRotationAxis: [1, 2], minSpeed: -1 }
  } });
  assert.deepEqual(project.damageAsset, damageAsset);
  assert.deepEqual(project.damageEdits.SCRATCHES, { minSpeed: 0, maxSpeed: 30 });
  assert.deepEqual(project.damageEdits.DAMAGE, { initialLevel: 40 });
  assert.deepEqual(project.damageEdits.VISUAL_OBJECT_0, { name: "HOOD_DAMAGE", staticRotationAxis: [1, 0, 0], damageZone: "FRONT", fullSpeed: 90 });
  assert.equal(project.damageEdits.VISUAL_OBJECT_1, undefined);
  assert.equal(editorProjectEditCount(project), 7);
  assert.equal(editorProjectCspEditCount(project), 0);
  assert.doesNotMatch(serializeEditorCsp(project), /damage/i);
  assert.equal(createEditorProject().damageAsset, null);
});

test("keeps stale car damage edits but drops their malformed identity", () => {
  const project = normalizeEditorProject({ format: "apex-editor-project", version: 1, damageAsset: { name: "damage.ini", size: 10, sha256: "short" }, damageEdits: { OSCILLATIONS: { enabled: false } } });
  assert.equal(project.damageAsset, null);
  assert.deepEqual(project.damageEdits.OSCILLATIONS, { enabled: false });
});

test("parses and formats scalar and vector editor values", () => {
  assert.equal(parseEditorValue("0.375"), 0.375);
  assert.deepEqual(parseEditorValue("1, 0.5, 0"), [1, 0.5, 0]);
  assert.equal(formatEditorValue([1, 0.333333333, 0]), "1, 0.333333, 0");
  assert.throws(() => parseEditorValue("1, nope"), /finite number/);
});

test("serializes deterministic CSP overrides that evaluate on matching materials", () => {
  const project = createEditorProject({ name: "car.kn5", size: 100, kn5Version: 6 });
  project.materialEdits.body = { shader: "smCarPaint", blendMode: "ALPHA_BLEND", properties: { ksSpecular: 0.8, ksDiffuse: 0.45 }, resources: { txMaps: { color: [1, 0.5, 0.25, 1] }, txDiffuse: { texture: "body.dds" }, txNormal: { file: "textures/body_nm.dds" } } };
  project.meshEdits.BODY = { isTransparent: true, layer: 9, lodOut: 500, castShadows: false };
  const text = serializeEditorCsp(project);
  assert.ok(text.indexOf("ksDiffuse") < text.indexOf("ksSpecular"));
  assert.ok(text.indexOf("txDiffuse") < text.indexOf("txMaps"));
  const mesh = { kind: "mesh", name: "BODY", materialId: 0, children: [] };
  const model = { materials: [{ name: "body", shader: "ksPerPixel", properties: [], resources: [] }], root: { kind: "node", name: "root", children: [mesh] } };
  const result = evaluateCspConfig(model, parseCspIni(text, "authored.ini"));
  const override = result.nodeOverrides.get(mesh);
  assert.equal(override.shader, "smCarPaint");
  assert.equal(override.blendMode, "ALPHA_BLEND");
  assert.equal(override.properties.get("ksdiffuse"), 0.45);
  assert.deepEqual(override.resources.get("txmaps").color, [1, 0.5, 0.25, 1]);
  assert.equal(override.resources.get("txdiffuse").texture, "body.dds");
  assert.equal(override.resources.get("txnormal").file, "textures/body_nm.dds");
  assert.equal(override.isTransparent, true);
  assert.equal(override.layer, 9);
  assert.equal(override.lodOut, 500);
  assert.equal(override.castShadows, false);
  assert.equal(JSON.parse(serializeEditorProject(project)).format, "apex-editor-project");
});
