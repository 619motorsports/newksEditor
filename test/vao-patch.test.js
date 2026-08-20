import assert from "node:assert/strict";
import { readFile } from "node:fs/promises";
import { join } from "node:path";
import test from "node:test";
import { parseKn5 } from "../src/kn5.js";
import { bindVaoPatch, CSP_VAO_BIND_DISTANCE_SQUARED, parseVaoData, parseVaoPatch } from "../src/vao-patch.js";
import { assettoPath, carFixtureRoot, carMainKn5 } from "./fixture-paths.js";

test("decodes native v4 square-root AO and v5 linear AO bytes", () => {
  const payload = vaoRecord("mesh", 1, [1, 2, 3], 3, Uint8Array.of(0, 64, 255));
  assert.deepEqual([...parseVaoData(payload, { version: 4 }).records[0].values], [0, 127, 255]);
  assert.deepEqual([...parseVaoData(payload, { version: 5 }).records[0].values], [0, 64, 255]);
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

test("parses installed CSP legacy, v4, and v5 ZIP fixtures", async (t) => {
  const extension = assettoPath("extension");
  let legacy, v4, v5;
  try { [legacy, v4, v5] = await Promise.all([readFile(`${extension}/vao-patches/ks_barcelona__layout_gp.vao-patch`), readFile(`${extension}/vao-patches-cars/ks_nissan_370z.vao-patch`), readFile(`${extension}/vao-patches-cars/ks_bmw_m4_akrapovic.vao-patch`)]); }
  catch { t.skip("Installed CSP VAO fixtures are unavailable"); return; }
  const barcelona = await parseVaoPatch(legacy, "ks_barcelona__layout_gp.vao-patch"), nissan = await parseVaoPatch(v4, "ks_nissan_370z.vao-patch"), bmw = await parseVaoPatch(v5, "ks_bmw_m4_akrapovic.vao-patch");
  assert.equal(barcelona.version, 1); assert.equal(barcelona.recordCount, 1945); assert.deepEqual([...barcelona.records[0].values.slice(8, 12)], [231, 203, 247, 217]);
  assert.equal(nissan.version, 4); assert.equal(nissan.recordCount, 908); assert.deepEqual([...nissan.records[0].values.slice(0, 4)], [234, 235, 228, 223]);
  assert.equal(bmw.version, 5); assert.ok(bmw.recordCount > 100); assert.equal(bmw.entry, "Patch_v5.data");
});

test("binds the repository car VAO patch to production KN5 geometry", async () => {
  const [patchBytes,kn5Bytes]=await Promise.all([readFile(join(carFixtureRoot,"main_geometry.vao-patch")),readFile(carMainKn5)]),patch=await parseVaoPatch(patchBytes,"main_geometry.vao-patch"),binding=bindVaoPatch(parseKn5(kn5Bytes),patch);
  assert.equal(patch.version,5);assert.equal(patch.recordCount,420);assert.equal(binding.matchedMeshes,27);assert.equal(binding.unmatchedRecords + binding.matchedRecords + binding.alternateRecords + binding.normalRecords, patch.recordCount);
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
