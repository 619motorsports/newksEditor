import assert from "node:assert/strict";
import { readFile } from "node:fs/promises";
import test from "node:test";
import { createSkinMetadata, MAX_SKIN_METADATA_BYTES, parseSkinMetadata, serializeSkinMetadata, SkinMetadataError } from "../src/skin-metadata.js";

test("parses official skin metadata field types and a UTF-8 BOM", () => {
  const parsed = parseSkinMetadata(new TextEncoder().encode('\uFEFF{"skinname":"Rosso","drivername":"","country":"Italy","team":"","number":33,"priority":30}'), "ui_skin.json");
  assert.deepEqual({ ...parsed.metadata }, { skinname: "Rosso", drivername: "", country: "Italy", team: "", number: "33", priority: 30 });
  assert.deepEqual(parsed.warnings, []);
});

test("rejects truncated, invalid UTF-8, oversized, and non-object metadata", () => {
  assert.throws(() => parseSkinMetadata('{"skinname":"Red"', "truncated.json"), (error) => error instanceof SkinMetadataError && /not valid JSON/.test(error.message));
  assert.throws(() => parseSkinMetadata(new Uint8Array([0xff, 0xfe])), /not valid UTF-8/);
  assert.throws(() => parseSkinMetadata(new Uint8Array(MAX_SKIN_METADATA_BYTES + 1)), /byte limit/);
  assert.throws(() => parseSkinMetadata("[]"), /one JSON object/);
});

test("preserves unknown fields and serializes edited standard fields", () => {
  const parsed = parseSkinMetadata('{"skinname":"Old","drivername":"Driver","country":"US","team":"Team","number":"7","priority":2,"extension":{"tag":true}}');
  const text = serializeSkinMetadata(parsed, { skinname: "New", number: "07", priority: 5 }), reparsed = JSON.parse(text);
  assert.deepEqual(reparsed, { skinname: "New", drivername: "Driver", country: "US", team: "Team", number: "07", priority: 5, extension: { tag: true } });
});

test("creates new metadata without inventing a priority", () => {
  assert.deepEqual(JSON.parse(serializeSkinMetadata(createSkinMetadata(), { skinname: "Blue" })), {
    skinname: "Blue", drivername: "", country: "", team: "", number: ""
  });
});

test("diagnoses invalid known fields and ignores unsafe keys", () => {
  const parsed = parseSkinMetadata('{"skinname":42,"priority":1.5,"__proto__":{"polluted":true}}');
  assert.equal(parsed.metadata.skinname, "");
  assert.equal(parsed.metadata.priority, null);
  assert.equal(Object.hasOwn(parsed.original, "__proto__"), false);
  assert.equal(parsed.warnings.length, 3);
  assert.equal({}.polluted, undefined);
});

test("parses an installed official Kunos skin", async (t) => {
  const path = "/mnt/D/SteamLibrary/steamapps/common/assettocorsa/content/cars/ks_alfa_33_stradale/skins/00_rosso/ui_skin.json";
  let bytes;
  try { bytes = await readFile(path); }
  catch { t.skip("The official Kunos skin fixture is not installed"); return; }
  const parsed = parseSkinMetadata(bytes, path);
  assert.deepEqual({ ...parsed.metadata }, { skinname: "Rosso", drivername: "", country: "", team: "", number: "", priority: 30 });
  assert.deepEqual(parsed.warnings, []);
});
