import assert from "node:assert/strict";
import test from "node:test";
import { createAssetFileIndex, discoverAssetAnimations, discoverAssetSkins, externalResourcePaths, matchSkinTextures, normalizeAssetPath, resolveAssetFile } from "../src/asset-files.js";

function file(name, webkitRelativePath = "") { return { name, webkitRelativePath }; }

test("normalizes CSP asset paths across platforms", () => {
  assert.equal(normalizeAssetPath(".\\extension\\textures\\..\\paint.dds"), "extension/paint.dds");
  assert.equal(normalizeAssetPath("/extension//textures/body.dds"), "extension/textures/body.dds");
  assert.equal(normalizeAssetPath("'?textures/common/water_normal.dds'"), "textures/common/water_normal.dds");
});

test("resolves folder files by project path, suffix, and unique basename", () => {
  const body = file("body.dds", "my_car/extension/textures/body.dds");
  const glass = file("glass.png", "my_car/skins/red/glass.png");
  const index = createAssetFileIndex([body, glass]);
  assert.equal(resolveAssetFile(index, "extension\\textures\\BODY.dds").file, body);
  assert.equal(resolveAssetFile(index, "textures/body.dds").matchedBy, "suffix");
  assert.equal(resolveAssetFile(index, "glass.png").file, glass);
  assert.equal(resolveAssetFile(index, "missing.dds").status, "missing");
  assert.equal(resolveAssetFile(createAssetFileIndex([file("loose.dds")]), "loose.dds").status, "resolved");
});

test("does not guess when a basename or suffix is ambiguous", () => {
  const first = file("shared.dds", "car/extension/a/shared.dds");
  const second = file("shared.dds", "car/extension/b/shared.dds");
  const result = resolveAssetFile(createAssetFileIndex([first, second]), "shared.dds");
  assert.equal(result.status, "ambiguous");
  assert.deepEqual(result.matches.map((entry) => entry.path), ["car/extension/a/shared.dds", "car/extension/b/shared.dds"]);
});

test("collects unique external resource files from evaluated overrides", () => {
  const overrides = new Map([
    [{}, { resources: new Map([["txdiffuse", { file: "textures/body.dds" }], ["txnormal", { file: "textures\\normal.dds" }]]) }],
    [{}, { resources: new Map([["txdiffuse", { file: "TEXTURES/body.dds" }], ["txmaps", { texture: "embedded.dds" }]]) }]
  ]);
  assert.deepEqual(externalResourcePaths({ nodeOverrides: overrides }), ["textures/body.dds", "textures/normal.dds"]);
});

test("discovers immediate skin texture folders and matches KN5 texture basenames", () => {
  const files = [
    file("car/skins/02_red/body.dds"), file("car/skins/02_red/Plate_D.dds"), file("car/skins/02_red/preview.jpg"),
    file("car/skins/10_blue/body.dds"), file("car/skins/10_blue/nested/ignored.dds"), file("car/skins/10_blue/ui_skin.json"),
    file("car/data/lods.ini")
  ];
  const skins = discoverAssetSkins(createAssetFileIndex(files));
  assert.deepEqual(skins.map((skin) => skin.name), ["02_red", "10_blue"]);
  assert.deepEqual(skins[0].files.map((entry) => entry.basename), ["body.dds", "Plate_D.dds", "preview.jpg"]);
  assert.deepEqual(skins[1].metadataFiles.map((entry) => entry.basename), ["ui_skin.json"]);
  const matched = matchSkinTextures(skins[0].files, new Set(["textures/BODY.DDS", "Plate_D.dds", "metal_detail.dds"]));
  assert.deepEqual(matched.files.map((entry) => entry.name), ["body.dds", "plate_d.dds"]);
  assert.deepEqual(matched.missing, ["metal_detail.dds"]);
});

test("reports case-colliding skin texture files as ambiguous", () => {
  const skin = discoverAssetSkins(createAssetFileIndex([file("car/skins/test/body.dds"), file("car/skins/test/BODY.DDS")]))[0];
  const matched = matchSkinTextures(skin.files, ["body.dds"]);
  assert.equal(matched.files.length, 0);
  assert.equal(matched.ambiguous[0].entries.length, 2);
});

test("discovers metadata-only skins and retains ambiguous metadata files", () => {
  const skins = discoverAssetSkins(createAssetFileIndex([
    file("car/skins/metadata_only/ui_skin.json"),
    file("car/skins/ambiguous/ui_skin.json"),
    file("car/skins/ambiguous/UI_SKIN.JSON")
  ]));
  assert.deepEqual(skins.map((skin) => [skin.name, skin.files.length, skin.metadataFiles.length]), [
    ["ambiguous", 0, 2], ["metadata_only", 0, 1]
  ]);
});

test("discovers animation files anywhere inside an asset folder", () => {
  const index = createAssetFileIndex([file("car/animations/steer.ksanim"), file("car/animations/shift.KSANIM"), file("car/data/lods.ini")]);
  assert.deepEqual(discoverAssetAnimations(index).map((entry) => entry.relativePath), ["animations/shift.KSANIM", "animations/steer.ksanim"]);
});
