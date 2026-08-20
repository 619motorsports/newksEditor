import assert from "node:assert/strict";
import { readFile } from "node:fs/promises";
import test from "node:test";
import { AcdError, createAcdKey, findAcdEntry, parseAcd } from "../src/acd.js";
import { assettoPath } from "./fixture-paths.js";

function int32Bytes(value) { const data = new Uint8Array(4); new DataView(data.buffer).setInt32(0, value, true); return data; }
function pack(assetName, files, header = null) {
  const password = createAcdKey(assetName).password, chunks = [];
  if (header !== null) chunks.push(int32Bytes(-1111), int32Bytes(header));
  for (const [name, value] of Object.entries(files)) {
    const nameBytes = new TextEncoder().encode(name), data = typeof value === "string" ? new TextEncoder().encode(value) : value, stored = new Uint8Array(data.length * 4);
    for (let index = 0; index < data.length; index++) stored[index * 4] = (data[index] + password.charCodeAt(index % password.length)) & 255;
    chunks.push(int32Bytes(nameBytes.length), nameBytes, int32Bytes(data.length), stored);
  }
  const length = chunks.reduce((sum, chunk) => sum + chunk.length, 0), output = new Uint8Array(length); let offset = 0;
  for (const chunk of chunks) { output.set(chunk, offset); offset += chunk.length; }
  return output;
}

test("derives the decimal ACD password with C# int32 behavior", () => {
  assert.deepEqual(createAcdKey("KS_AUDI_SPORT_QUATTRO").octets, [230, 190, 101, 8, 154, 33, 64, 112]);
  assert.equal(createAcdKey("KS_AUDI_SPORT_QUATTRO").password, "230-190-101-8-154-33-64-112");
});

test("reads headered and unheadered ACD records as bytes", () => {
  for (const header of [null, 243591]) {
    const archive = parseAcd(pack("example_car", { "car.ini": "[INFO]\nSCREEN_NAME=Example\n", "sub/file.bin": new Uint8Array([0, 255, 17]) }, header), "example_car");
    assert.equal(archive.header, header);
    assert.equal(new TextDecoder().decode(findAcdEntry(archive, "CAR.INI").data), "[INFO]\nSCREEN_NAME=Example\n");
    assert.deepEqual(findAcdEntry(archive, "sub\\file.bin").data, new Uint8Array([0, 255, 17]));
    assert.equal(archive.bytesRead, archive.byteLength);
  }
});

test("does not index unsafe or duplicate archive paths", () => {
  const archive = parseAcd(pack("example_car", { "../escape.ini": "bad", "Car.ini": "first", "car.ini": "second" }), "example_car");
  assert.equal(archive.entries.length, 3);
  assert.equal(archive.byPath.size, 1);
  assert.match(archive.warnings.join("\n"), /unsafe archive path/);
  assert.match(archive.warnings.join("\n"), /duplicate archive path/);
  assert.equal(new TextDecoder().decode(findAcdEntry(archive, "car.ini").data), "first");
});

test("rejects truncated ACD records with their byte offset", () => {
  const data = pack("example_car", { "car.ini": "value" });
  assert.throws(() => parseAcd(data.subarray(0, data.length - 1), "example_car"), (error) => error instanceof AcdError && /Truncated payload/.test(error.message) && error.offset > 0);
});

test("opens an installed official Kunos car data.acd", async (t) => {
  const path = assettoPath("content/cars/ks_nissan_370z/data.acd");
  let data; try { data = await readFile(path); } catch { t.skip("Assetto Corsa data.acd fixture is not installed"); return; }
  const archive = parseAcd(data, "ks_nissan_370z", path), car = findAcdEntry(archive, "car.ini"), lods = findAcdEntry(archive, "lods.ini");
  assert.equal(archive.bytesRead, data.length);
  assert.ok(archive.entries.length > 20);
  assert.match(new TextDecoder().decode(car.data), /^\[HEADER\][\s\S]*\[INFO\]/);
  assert.match(new TextDecoder().decode(lods.data), /^\[COCKPIT_HR\][\s\S]*\[LOD_0\]/);
});

test("matches an installed archive entry byte-for-byte with its unpacked copy", async (t) => {
  const directory = assettoPath("content/cars/2009_nascar");
  let packed, unpacked; try { [packed, unpacked] = await Promise.all([readFile(`${directory}/data.acd`), readFile(`${directory}/data/lods.ini`)]); } catch { t.skip("Matching packed and unpacked fixtures are not installed"); return; }
  const archive = parseAcd(packed, "2009_nascar", `${directory}/data.acd`), lods = findAcdEntry(archive, "lods.ini");
  assert.ok(lods);
  assert.deepEqual(lods.data, new Uint8Array(unpacked));
});
