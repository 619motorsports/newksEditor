import assert from "node:assert/strict";
import { readFile } from "node:fs/promises";
import test from "node:test";
import { parseKn5, walkNodes } from "../src/kn5.js";
import { auditStockTyreMaterials, isStockTyreShader, normalizeTyrePreviewLevel, stockTyreFresnelCap, stockTyreNormal, stockTyreSpecular, stockTyreTexel } from "../src/tyre-shader.js";
import { assettoPath } from "./fixture-paths.js";

test("recognizes only the two stock tyre shader packages", () => {
  assert.equal(isStockTyreShader("ksTyres"), true);
  assert.equal(isStockTyreShader("newStefano_ksTyres"), true);
  assert.equal(isStockTyreShader("ksBrakeDisc"), false);
  assert.equal(isStockTyreShader("ksPerPixel"), false);
});

test("matches the stock tyre diffuse, blur, and dirt operations", () => {
  const texel = stockTyreTexel([0.2, 0.4, 0.6, 0.8], [0.8, 0.6, 0.4, 0.2], [1, 0, 0.5, 0.25], 0.5, 0.8);
  assert.deepEqual(texel.map((value) => Number(value.toFixed(6))), [0.6, 0.4, 0.5, 0.5]);
  assert.deepEqual(stockTyreTexel([0.2, 0.4, 0.6, 0.8], [0.8, 0.6, 0.4, 0.2], [1, 0, 0.5, 1], 0, 0), [0.2, 0.4, 0.6, 0.8]);
});

test("matches the stock unnormalized blend of two mapped tyre normals", () => {
  const normal = stockTyreNormal([0.5, 0.5, 1], [1, 0.5, 0.5], [1, 0, 0], [0, 1, 0], [0, 0, 1], 0.5);
  assert.deepEqual(normal, [0.5, 0, 0.5]);
  assert.ok(Math.abs(Math.hypot(...normal) - Math.SQRT1_2) < 1e-12);
});

test("matches the stock dirt reductions for specular and reflections", () => {
  assert.equal(stockTyreSpecular(0.8, 0.25, 0.5), 0.1);
  assert.equal(stockTyreFresnelCap(0.08, 0.25), 0.06);
  assert.equal(stockTyreSpecular(0.8, 0.25, 1), 0);
  assert.equal(stockTyreFresnelCap(0.08, 1), 0);
});

test("requires usable textures before auditing a tyre binding as exact", () => {
  const materials = [
    { name: "Front tyres", shader: "ksTyres", resources: ["txDiffuse", "txNormal", "txDirty", "txBlur", "txNormalBlur"].map((slot) => ({ slot, texture: { usable: slot !== "txNormal" } })) },
    { name: "Rear tyres", shader: "newStefano_ksTyres", resources: [{ slot: "txDiffuse" }] },
    { name: "Discs", shader: "ksBrakeDisc", resources: [{ slot: "txBlur" }] }
  ];
  const declarations = auditStockTyreMaterials(materials);
  assert.equal(declarations.declaredCompleteMaterials, 1);
  assert.equal(declarations.completeMaterials, 0);
  assert.equal(declarations.entries[0].verified, false);
  const audit = auditStockTyreMaterials(materials, (_material, resource) => resource.texture?.usable === true);
  assert.equal(audit.materials, 2);
  assert.equal(audit.completeMaterials, 0);
  assert.equal(audit.incompleteMaterials, 2);
  assert.deepEqual(audit.entries[0].missingResources, ["txNormal"]);
  assert.deepEqual(audit.entries[1].missingResources, ["txDiffuse", "txNormal", "txDirty", "txBlur", "txNormalBlur"]);
  assert.throws(() => auditStockTyreMaterials(materials, true), /validation must be a function/);
});

test("rejects malformed preview state and vector input", () => {
  for (const value of [-0.01, 1.01, Infinity, NaN, "no", "", null]) assert.throws(() => normalizeTyrePreviewLevel(value));
  assert.throws(() => stockTyreTexel([1, 1, 1], [1, 1, 1, 1], [1, 1, 1, 1], 0, 0), /4 components/);
  assert.throws(() => stockTyreTexel("1111", [1, 1, 1, 1], [1, 1, 1, 1], 0, 0), /4 components/);
  assert.throws(() => stockTyreNormal([0.5, 0.5, 1], [0.5, 0.5, 1], [0, 0, 0], [0, 1, 0], [0, 0, 1], 0), /zero length/);
});

test("audits the installed Porsche stock tyre material", async (context) => {
  let bytes;
  try {
    bytes = await readFile(assettoPath("content/cars/ks_porsche_917_30/porsche_917_30.kn5"));
  } catch {
    context.skip("Installed Porsche tyre fixture is unavailable");
    return;
  }
  const model = parseKn5(bytes), audit = auditStockTyreMaterials(model.materials), materialId = audit.entries[0]?.materialId;
  const meshNames = walkNodes(model.root).filter(({ node }) => (node.kind === "mesh" || node.kind === "skinnedMesh") && node.materialId === materialId).map(({ node }) => node.name).sort();
  assert.equal(audit.materials, 1);
  assert.equal(audit.declaredCompleteMaterials, 1);
  assert.equal(audit.completeMaterials, 0);
  assert.deepEqual(audit.entries[0].missingResources, []);
  assert.deepEqual(meshNames, ["TYRE_MESH_LF", "TYRE_MESH_LR", "TYRE_MESH_RF", "TYRE_MESH_RR"]);
});
