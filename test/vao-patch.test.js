import assert from "node:assert/strict";
import { readFile } from "node:fs/promises";
import test from "node:test";
import { parseKn5 } from "../src/kn5.js";
import { bindVaoPatch, CSP_VAO_BIND_DISTANCE_SQUARED, parseSplitAoConfig, parseVaoData, parseVaoPatch, resolveSplitAoAnimation, splitAoAnimationNodeScope, splitAoBindingAmount } from "../src/vao-patch.js";

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

test("binds records by exact name, vertex count, and first-position tolerance", () => {
  const node = mesh("body", [[1, 2, 3], [4, 5, 6]]), model = { root: { kind: "node", name: "root", active: true, children: [node] } };
  const match = { name: "body", type: 1, channel: "primary", alternate: false, firstVertex: [1.05, 2, 3], vertexCount: 2, values: Uint8Array.of(100, 200) };
  const result = bindVaoPatch(model, { records: [match], recordCount: 1 });
  assert.equal(result.matchedMeshes, 1); assert.equal(result.vertices, 2); assert.equal(result.mean, 150);
  assert.ok((.05 ** 2) < CSP_VAO_BIND_DISTANCE_SQUARED);
  const miss = bindVaoPatch(model, { records: [{ ...match, firstVertex: [1.1, 2, 3] }], recordCount: 1 });
  assert.equal(miss.matchedMeshes, 0);
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
  const split = parseSplitAoConfig("[SPLIT_AO]\nDOOR_NODES=DOOR_L,DOOR_R"), state = resolveSplitAoAnimation(split, "car_door_L.ksanim", .5, scope.tracks, scope.related);
  const leftBinding = { secondary: Uint8Array.of(20), nodeNames: ["COCKPIT_HR", "DOOR_L", "LEFT_MESH"] };
  const rightBinding = { secondary: Uint8Array.of(20), nodeNames: ["COCKPIT_HR", "DOOR_R", "RIGHT_MESH"] };

  assert.deepEqual(scope, { tracks: ["door_l"], related: ["door_l", "cockpit_hr"] });
  assert.deepEqual([...state.nodes], ["door_l"]);
  assert.equal(splitAoBindingAmount(leftBinding, state), .25);
  assert.equal(splitAoBindingAmount(rightBinding, state), 0);
});

test("parses installed CSP legacy, v4, and v5 ZIP fixtures", async (t) => {
  const extension = "/mnt/D/SteamLibrary/steamapps/common/assettocorsa/extension";
  let legacy, v4, v5;
  try { [legacy, v4, v5] = await Promise.all([readFile(`${extension}/vao-patches/ks_barcelona__layout_gp.vao-patch`), readFile(`${extension}/vao-patches-cars/ks_nissan_370z.vao-patch`), readFile(`${extension}/vao-patches-cars/ks_bmw_m4_akrapovic.vao-patch`)]); }
  catch { t.skip("Installed CSP VAO fixtures are unavailable"); return; }
  const barcelona = await parseVaoPatch(legacy, "ks_barcelona__layout_gp.vao-patch"), nissan = await parseVaoPatch(v4, "ks_nissan_370z.vao-patch"), bmw = await parseVaoPatch(v5, "ks_bmw_m4_akrapovic.vao-patch");
  assert.equal(barcelona.version, 1); assert.equal(barcelona.recordCount, 1945); assert.deepEqual([...barcelona.records[0].values.slice(8, 12)], [231, 203, 247, 217]);
  assert.equal(nissan.version, 4); assert.equal(nissan.recordCount, 908); assert.deepEqual([...nissan.records[0].values.slice(0, 4)], [234, 235, 228, 223]);
  assert.equal(nissan.splitAo.present, true); assert.equal(nissan.splitAo.door.exponent, 2); assert.equal(nissan.splitAo.wings[0].name, "car_rear_wing.ksanim");
  assert.equal(bmw.version, 5); assert.ok(bmw.recordCount > 100); assert.equal(bmw.entry, "Patch_v5.data");
});

test("binds an installed CSP car VAO patch to production KN5 geometry", async (t) => {
  const car = "/mnt/D/SteamLibrary/steamapps/common/assettocorsa/content/cars/ks_nissan_370z", patchPath = "/mnt/D/SteamLibrary/steamapps/common/assettocorsa/extension/vao-patches-cars/ks_nissan_370z.vao-patch";
  let patchBytes, kn5Bytes;
  try { [patchBytes, kn5Bytes] = await Promise.all([readFile(patchPath), readFile(`${car}/nissan_370z.kn5`)]); }
  catch { t.skip("Installed Nissan VAO/KN5 fixtures are unavailable"); return; }
  const patch = await parseVaoPatch(patchBytes, "ks_nissan_370z.vao-patch"), binding = bindVaoPatch(parseKn5(kn5Bytes), patch);
  assert.ok(binding.matchedMeshes > 100); assert.equal(binding.unmatchedRecords + binding.matchedRecords + binding.alternateRecords + binding.normalRecords, patch.recordCount);
  assert.ok(binding.secondaryMeshes > 100); assert.ok([...binding.bindings.values()].some((entry) => entry.secondary && entry.nodeNames.includes("COCKPIT_HR")));
  assert.ok(binding.minimum < binding.maximum); assert.ok(binding.mean > 0 && binding.mean < 255);
});

function vaoRecord(name, type, first, count, payload) {
  const nameBytes = new TextEncoder().encode(name), bytes = new Uint8Array(4 + nameBytes.length + 4 + 12 + 4 + payload.length), view = new DataView(bytes.buffer); let offset = 0;
  view.setUint32(offset, nameBytes.length, true); offset += 4; bytes.set(nameBytes, offset); offset += nameBytes.length;
  view.setUint32(offset, type, true); offset += 4; for (const value of first) { view.setFloat32(offset, value, true); offset += 4; }
  view.setUint32(offset, count, true); offset += 4; bytes.set(payload, offset); return bytes;
}
function halfBytes(values) { const bytes = new Uint8Array(values.length * 2), view = new DataView(bytes.buffer); values.forEach((value, index) => view.setUint16(index * 2, value === 0 ? 0 : value === .5 ? 0x3800 : 0x3c00, true)); return bytes; }
function mesh(name, positions) { const stride = 11, vertices = new Float32Array(positions.length * stride); positions.forEach((position, index) => vertices.set(position, index * stride)); return { kind: "mesh", name, active: true, visible: true, renderable: true, vertexStride: stride, vertices, indices: new Uint16Array(), children: [] }; }
