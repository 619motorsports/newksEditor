import assert from "node:assert/strict";
import test from "node:test";
import { createEditorProject, editorProjectCspEditCount, editorProjectEditCount, formatEditorValue, normalizeEditorProject, parseEditorValue, serializeEditorCsp, serializeEditorProject } from "../src/editor-project.js";
import { evaluateCspConfig, parseCspIni } from "../src/csp-config.js";

test("normalizes portable projects and rejects incompatible input", () => {
  const project = normalizeEditorProject({ format: "apex-editor-project", version: 1, asset: { name: "car.kn5", size: 42, kn5Version: 6 }, materialEdits: { body: { shader: "smCarPaint", properties: { ksDiffuse: 0.5, ksEmissive: [1, 0, 0] }, resources: { txMaps: { color: [1, 1, 1, 1] } } } }, meshEdits: { BODY: { isTransparent: false, layer: 3 } }, nodeEdits: { "0": { name: "BODY_RENAMED", active: false, transform: [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 2, 3, 4, 1] } }, geometryEdits: { "0/0": { transform: [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, -1, 2, 3, 1] } }, workspaceEdits: { files: { "1": { lodIn: 15, lodOut: 45 } }, cockpitHrDistance: 6 } });
  assert.equal(project.asset.name, "car.kn5");
  assert.equal(editorProjectEditCount(project), 13);
  assert.equal(editorProjectCspEditCount(project), 6);
  assert.equal(project.nodeEdits["0"].name, "BODY_RENAMED");
  assert.equal(project.nodeEdits["0"].active, false);
  assert.deepEqual(project.nodeEdits["0"].transform.slice(12, 15), [2, 3, 4]);
  assert.deepEqual(project.geometryEdits["0/0"].transform.slice(12, 15), [-1, 2, 3]);
  assert.deepEqual(project.workspaceEdits.files["1"], { lodIn: 15, lodOut: 45 });
  assert.equal(project.workspaceEdits.cockpitHrDistance, 6);
  assert.throws(() => normalizeEditorProject({ format: "other", version: 1 }), /Not an Apex Editor project/);
  assert.throws(() => normalizeEditorProject({ format: "apex-editor-project", version: 99 }), /Unsupported/);
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
