import assert from "node:assert/strict";
import { readFile } from "node:fs/promises";
import { join } from "node:path";
import test from "node:test";
import { parseKn5 } from "../src/kn5.js";
import { parseKsAnimation } from "../src/ksanim.js";
import { bindVaoPatch, CSP_VAO_BIND_DISTANCE_SQUARED, parseSplitAoConfig, parseVaoData, parseVaoPatch, resolveSplitAoAnimation, splitAoAnimationNodeScope, splitAoBindingAmount } from "../src/vao-patch.js";
import { assettoPath, carFixtureRoot, carMainKn5 } from "./fixture-paths.js";

test("decodes native v4 square-root AO and v5 linear AO bytes", () => {
  const payload = vaoRecord("mesh", 1, [1, 2, 3], 3, Uint8Array.of(0, 64, 255));
  assert.deepEqual([...parseVaoData(payload, { version: 4 }).records[0].values], [0, 127, 255]);
  assert.deepEqual([...parseVaoData(payload, { version: 5 }).records[0].values], [0, 64, 255]);
  assert.throws(() => parseVaoData(payload.subarray(0, payload.length - 1), { version: 5, source: "truncated.data" }), /truncated\.data: truncated record mesh/);
});

test("decodes legacy half-float AO with lighting controls", () => {
  const payload = vaoRecord("mesh", 1, [0, 0, 0], 3, halfBytes([0, .5, 1]));
  const patch = parseVaoData(payload, { version: 1, lighting: { opacity: 1, brightness: 1, gamma: 1 } });
  assert.deepEqual([...patch.records[0].values], [0, 179, 255]);
});

test("decodes and normalizes native VAO normal overrides", () => {
  const legacy = parseVaoData(vaoRecord("legacy", 2, [0, 0, 0], 2, halfBytes([1, 1, 0, 0, 1, 0])), { version: 1 });
  assert.deepEqual([...legacy.records[0].values].map((value) => Number(value.toFixed(6))), [.707107, .707107, 0, 0, 1, 0]);

  const modern = parseVaoData(vaoRecord("modern", 2, [0, 0, 0], 1, Uint8Array.of(255, 128, 0)), { version: 5 });
  const expectedX = 1 / Math.hypot(1, (128 / 255) ** 2), expectedY = (128 / 255) ** 2 / Math.hypot(1, (128 / 255) ** 2);
  assert.ok(Math.abs(modern.records[0].values[0] - expectedX) < 1e-6);
  assert.ok(Math.abs(modern.records[0].values[1] - expectedY) < 1e-6);
  assert.equal(modern.records[0].values[2], 0);
});

test("rejects malformed VAO normal payloads", () => {
  const payload = vaoRecord("mesh", 2, [0, 0, 0], 1, halfBytes([1, 0, 0]));
  assert.throws(() => parseVaoData(payload.subarray(0, payload.length - 1), { version: 1, source: "truncated.data" }), /truncated\.data: truncated record mesh/);
  assert.throws(() => parseVaoData(vaoRecord("mesh", 2, [0, 0, 0], 1, halfBytes([0, 0, 0])), { version: 1, source: "zero.data" }), /zero\.data: invalid normal for mesh at vertex 0/);
  assert.throws(() => parseVaoData(vaoRecord("mesh", 2, [0, 0, 0], 1, halfWords([0x7c00, 0, 0])), { version: 1, source: "infinite.data" }), /infinite\.data: invalid normal for mesh at vertex 0/);
});

test("binds records by exact name, vertex count, and first-position tolerance", () => {
  const node = mesh("body", [[1, 2, 3], [4, 5, 6]]), model = { root: { kind: "node", name: "root", active: true, children: [node] } };
  const match = { name: "body", type: 1, channel: "primary", alternate: false, firstVertex: [1.05, 2, 3], vertexCount: 2, values: Uint8Array.of(100, 200) };
  const result = bindVaoPatch(model, { records: [match], recordCount: 1 });
  assert.equal(result.matchedMeshes, 1); assert.equal(result.vertices, 2); assert.equal(result.mean, 150);
  assert.ok((.05 ** 2) < CSP_VAO_BIND_DISTANCE_SQUARED);
  const miss = bindVaoPatch(model, { records: [{ ...match, firstVertex: [1.1, 2, 3] }], recordCount: 1 });
  assert.equal(miss.matchedMeshes, 0);
});

test("binds normal overrides with the native mesh identity key", () => {
  const node = mesh("road", [[1, 2, 3], [4, 5, 6]]), model = { root: { kind: "node", name: "root", active: true, children: [node] } };
  const values = Float32Array.of(0, 1, 0, 0, 1, 0), normal = { name: "road", type: 2, channel: "normal", alternate: false, firstVertex: [1, 2, 3], vertexCount: 2, values };
  const result = bindVaoPatch(model, { records: [normal], recordCount: 1 });
  assert.equal(result.matchedMeshes, 1);
  assert.equal(result.normalRecords, 1);
  assert.equal(result.matchedNormalRecords, 1);
  assert.equal(result.unmatchedNormalRecords, 0);
  assert.equal(result.normalMeshes, 1);
  assert.equal(result.normalVertices, 2);
  assert.equal(result.bindings.get(node).normal, values);
});

test("parses native split-AO groups and contiguous wing animations", () => {
  const split = parseSplitAoConfig(`
    [SPLIT_AO]
    COCKPIT_HR=COCKPIT_HR
    DOOR_EXP=2.5
    DOOR_NODES=DOOR_L, COCKPIT_HR, door_l
    HEADLIGHTS_NODES=@AUTO
    STEERING_WHEEL_NODES=STEER_HR,STEER_LR
    WING_ANIM_0_NAME=rear_wing.ksanim
    WING_ANIM_0_EXP=1.25
    WING_ANIM_0_NODES=WING_ROOT,@AUTO
    WING_ANIM_2_NAME=ignored_gap.ksanim
  `);
  assert.equal(split.present, true);
  assert.deepEqual(split.door, { exponent: 2.5, nodes: ["DOOR_L", "COCKPIT_HR"] });
  assert.deepEqual(split.headlights, { exponent: 2, nodes: ["@AUTO"] });
  assert.deepEqual(split.steeringWheel.nodes, ["STEER_HR", "STEER_LR"]);
  assert.deepEqual(split.wings, [{ index: 0, name: "rear_wing.ksanim", exponent: 1.25, nodes: ["WING_ROOT", "@AUTO"] }]);
  assert.deepEqual(split.warnings, []);
});

test("uses safe split-AO defaults for malformed configuration values", () => {
  const tooLong = "x".repeat(1025), split = parseSplitAoConfig(`[SPLIT_AO]\nDOOR_EXP=not-a-number\nHEADLIGHTS_EXP=-2\nDOOR_NODES=${tooLong}\nWING_ANIM_0_NAME=${tooLong}`);
  assert.equal(split.door.exponent, 2);
  assert.equal(split.headlights.exponent, 2);
  assert.deepEqual(split.door.nodes, []);
  assert.deepEqual(split.wings, []);
  assert.equal(split.warnings.length, 4);
});

test("previews the configured power curve from secondary bind-pose AO to primary animation AO", () => {
  const split = parseSplitAoConfig("[SPLIT_AO]\nDOOR_EXP=2\nDOOR_NODES=DOOR_ROOT,@AUTO"), state = resolveSplitAoAnimation(split, "animations/car_door_L.ksanim", .5, ["AUTO_CHILD"], ["DOOR_ROOT", "AUTO_CHILD"]);
  const binding = { secondary: Uint8Array.of(20), primary: Uint8Array.of(220), nodeNames: ["ROOT", "DOOR_ROOT", "MESH"] };
  assert.equal(state.kind, "door");
  assert.equal(state.amount, .25);
  assert.deepEqual([...state.nodes], ["door_root", "auto_child"]);
  assert.equal(splitAoBindingAmount(binding, state), .25);
  assert.equal(splitAoBindingAmount({ ...binding, nodeNames: ["ROOT", "OTHER"] }, state), 0);
  assert.equal(resolveSplitAoAnimation(split, "animations/car_shift.ksanim", 1).amount, 0);
});

test("restricts shared door split AO to the selected animated subtree", () => {
  const leftMesh = mesh("LEFT_MESH", [[0, 0, 0]]), rightMesh = mesh("RIGHT_MESH", [[0, 0, 0]]);
  const left = { kind: "node", name: "DOOR_L", children: [leftMesh] }, right = { kind: "node", name: "DOOR_R", children: [rightMesh] };
  const root = { kind: "node", name: "COCKPIT_HR", children: [left, right] };
  const scope = splitAoAnimationNodeScope(root, { tracks: [
    { name: "DOOR_L", animated: true, frames: [{}] },
    { name: "DOOR_R", animated: false, frames: [{}] }
  ] });
  const split = parseSplitAoConfig("[SPLIT_AO]\nDOOR_NODES=DOOR_L,DOOR_R"), state = resolveSplitAoAnimation(split, "car_door_L.ksanim", .5, scope.tracks, scope.related, scope.paths);
  const leftBinding = { secondary: Uint8Array.of(20), nodeNames: ["COCKPIT_HR", "DOOR_L", "LEFT_MESH"] };
  const rightBinding = { secondary: Uint8Array.of(20), nodeNames: ["COCKPIT_HR", "DOOR_R", "RIGHT_MESH"] };

  assert.deepEqual(scope, { tracks: ["door_l"], related: ["door_l", "cockpit_hr"], paths: [["cockpit_hr", "door_l"]] });
  assert.deepEqual([...state.nodes], ["door_l"]);
  assert.deepEqual(state.branches, [["cockpit_hr", "door_l"]]);
  assert.equal(splitAoBindingAmount(leftBinding, state), .25);
  assert.equal(splitAoBindingAmount(rightBinding, state), 0);
});

test("does not apply shared-ancestor door AO to sibling branches", () => {
  const left = { kind: "node", name: "DOOR_L", children: [mesh("LEFT_MESH", [[0, 0, 0]])] };
  const right = { kind: "node", name: "DOOR_R", children: [mesh("RIGHT_MESH", [[0, 0, 0]])] };
  const root = { kind: "node", name: "COCKPIT_HR", children: [left, right, mesh("DASH", [[0, 0, 0]])] };
  const scope = splitAoAnimationNodeScope(root, { tracks: [{ name: "DOOR_L", animated: true, frames: [{}] }] });
  const split = parseSplitAoConfig("[SPLIT_AO]\nDOOR_NODES=COCKPIT_HR");
  const state = resolveSplitAoAnimation(split, "car_door_L.ksanim", .5, scope.tracks, scope.related, scope.paths);

  assert.deepEqual(state.branches, [["cockpit_hr", "door_l"]]);
  assert.equal(splitAoBindingAmount({ secondary: Uint8Array.of(20), nodeNames: ["COCKPIT_HR", "DOOR_L", "LEFT_MESH"] }, state), .25);
  assert.equal(splitAoBindingAmount({ secondary: Uint8Array.of(20), nodeNames: ["COCKPIT_HR", "DOOR_R", "RIGHT_MESH"] }, state), 0);
  assert.equal(splitAoBindingAmount({ secondary: Uint8Array.of(20), nodeNames: ["COCKPIT_HR", "DASH"] }, state), 0);
});

test("parses installed CSP legacy, v4, and v5 ZIP fixtures", async (t) => {
  const extension = assettoPath("extension");
  let legacy, v4, v5;
  try { [legacy, v4, v5] = await Promise.all([readFile(`${extension}/vao-patches/ks_barcelona__layout_gp.vao-patch`), readFile(`${extension}/vao-patches-cars/ks_nissan_370z.vao-patch`), readFile(`${extension}/vao-patches-cars/ks_bmw_m4_akrapovic.vao-patch`)]); }
  catch { t.skip("Installed CSP VAO fixtures are unavailable"); return; }
  const barcelona = await parseVaoPatch(legacy, "ks_barcelona__layout_gp.vao-patch"), nissan = await parseVaoPatch(v4, "ks_nissan_370z.vao-patch"), bmw = await parseVaoPatch(v5, "ks_bmw_m4_akrapovic.vao-patch");
  assert.equal(barcelona.version, 1); assert.equal(barcelona.recordCount, 1945); assert.deepEqual([...barcelona.records[0].values.slice(8, 12)], [231, 203, 247, 217]);
  const barcelonaNormal = barcelona.records.find((record) => record.channel === "normal");
  assert.ok(barcelonaNormal); assert.equal(barcelonaNormal.values.length, barcelonaNormal.vertexCount * 3); assert.ok(Math.abs(Math.hypot(...barcelonaNormal.values.slice(0, 3)) - 1) < 1e-6);
  assert.equal(nissan.version, 4); assert.equal(nissan.recordCount, 908); assert.deepEqual([...nissan.records[0].values.slice(0, 4)], [234, 235, 228, 223]);
  assert.equal(nissan.splitAo.present, true); assert.equal(nissan.splitAo.door.exponent, 2); assert.equal(nissan.splitAo.wings[0].name, "car_rear_wing.ksanim");
  assert.equal(bmw.version, 5); assert.ok(bmw.recordCount > 100); assert.equal(bmw.entry, "Patch_v5.data");
});

test("binds the repository car VAO patch to production KN5 geometry", async () => {
  const [patchBytes, kn5Bytes] = await Promise.all([readFile(join(carFixtureRoot, "main_geometry.vao-patch")), readFile(carMainKn5)]), patch = await parseVaoPatch(patchBytes, "main_geometry.vao-patch"), binding = bindVaoPatch(parseKn5(kn5Bytes), patch);
  assert.equal(patch.version, 5); assert.equal(patch.recordCount, 420); assert.equal(binding.matchedMeshes, 27); assert.equal(binding.unmatchedRecords + binding.matchedRecords + binding.alternateRecords + binding.normalRecords, patch.recordCount);
  assert.ok(binding.minimum < binding.maximum); assert.ok(binding.mean > 0 && binding.mean < 255);
});

test("scopes installed Nissan split AO to the animated door subtree", async (t) => {
  const car = assettoPath("content/cars/ks_nissan_370z"), patchPath = assettoPath("extension/vao-patches-cars/ks_nissan_370z.vao-patch");
  let patchBytes, kn5Bytes, animationBytes;
  try { [patchBytes, kn5Bytes, animationBytes] = await Promise.all([readFile(patchPath), readFile(`${car}/nissan_370z.kn5`), readFile(`${car}/animations/car_door_L.ksanim`)]); }
  catch { t.skip("Installed Nissan VAO/KN5 fixtures are unavailable"); return; }
  const patch = await parseVaoPatch(patchBytes, "ks_nissan_370z.vao-patch"), model = parseKn5(kn5Bytes), binding = bindVaoPatch(model, patch);
  assert.ok(binding.matchedMeshes > 100); assert.equal(binding.unmatchedRecords + binding.matchedRecords + binding.alternateRecords + binding.normalRecords, patch.recordCount);
  assert.ok(binding.secondaryMeshes > 100); assert.ok([...binding.bindings.values()].some((entry) => entry.secondary && entry.nodeNames.includes("COCKPIT_HR")));
  assert.ok(binding.minimum < binding.maximum); assert.ok(binding.mean > 0 && binding.mean < 255);
  const animation = parseKsAnimation(animationBytes), scope = splitAoAnimationNodeScope(model.root, animation);
  const state = resolveSplitAoAnimation(patch.splitAo, "car_door_L.ksanim", .5, scope.tracks, scope.related, scope.paths);
  const active = [...binding.bindings.values()].filter((entry) => splitAoBindingAmount(entry, state) > 0);
  assert.equal(active.length, 17);
  assert.ok(active.some((entry) => entry.nodeNames.includes("DOOR_L")));
  assert.ok(active.every((entry) => !entry.nodeNames.some((name) => /^DOOR_R(?:_|$)/i.test(name))));
});

function vaoRecord(name, type, first, count, payload) {
  const nameBytes = new TextEncoder().encode(name), bytes = new Uint8Array(4 + nameBytes.length + 4 + 12 + 4 + payload.length), view = new DataView(bytes.buffer); let offset = 0;
  view.setUint32(offset, nameBytes.length, true); offset += 4; bytes.set(nameBytes, offset); offset += nameBytes.length;
  view.setUint32(offset, type, true); offset += 4; for (const value of first) { view.setFloat32(offset, value, true); offset += 4; }
  view.setUint32(offset, count, true); offset += 4; bytes.set(payload, offset); return bytes;
}
function halfBytes(values) { const bytes = new Uint8Array(values.length * 2), view = new DataView(bytes.buffer); values.forEach((value, index) => view.setUint16(index * 2, value === 0 ? 0 : value === .5 ? 0x3800 : 0x3c00, true)); return bytes; }
function halfWords(values) { const bytes = new Uint8Array(values.length * 2), view = new DataView(bytes.buffer); values.forEach((value, index) => view.setUint16(index * 2, value, true)); return bytes; }
function mesh(name, positions) { const stride = 11, vertices = new Float32Array(positions.length * stride); positions.forEach((position, index) => vertices.set(position, index * stride)); return { kind: "mesh", name, active: true, visible: true, renderable: true, vertexStride: stride, vertices, indices: new Uint16Array(), children: [] }; }
