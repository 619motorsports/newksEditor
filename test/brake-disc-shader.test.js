import assert from "node:assert/strict";
import { readFile } from "node:fs/promises";
import test from "node:test";
import { parseKn5, walkNodes } from "../src/kn5.js";
import { auditStockBrakeDiscMaterials, brakeDiscGlowStep, brakeDiscGlowTarget, isStockBrakeDiscShader, normalizeBrakeDiscBlur, parseBrakeDiscConfig, resolveBrakeDiscWheel, stockBrakeDiscGlow, stockBrakeDiscNormal, stockBrakeDiscTexel } from "../src/brake-disc-shader.js";
import { parseAcd, findAcdEntry } from "../src/acd.js";
import { assettoPath } from "./fixture-paths.js";

test("parses stock brake-disc configuration and diagnoses malformed input", () => {
  const parsed = parseBrakeDiscConfig(`[DISCS_GRAPHICS]\nDISC_LF=DISC_LF\nDISC_RF=DISC_RF\nDISC_LR=DISC_LR\nDISC_RR=DISC_RR\nFRONT_MAX_GLOW=64\nREAR_MAX_GLOW=24\nLAG_HOT=0.991\nLAG_COOL=0.97\n`);
  assert.deepEqual(parsed.nodes, { LF: "DISC_LF", RF: "DISC_RF", LR: "DISC_LR", RR: "DISC_RR" });
  assert.deepEqual([parsed.frontMaxGlow, parsed.rearMaxGlow, parsed.lagHot, parsed.lagCool], [64, 24, 0.991, 0.97]);
  assert.deepEqual(parsed.warnings, []);

  const malformed = parseBrakeDiscConfig(`[DISCS_GRAPHICS]\nDISC_LF=DISC_LF\nFRONT_MAX_GLOW=broken\nREAR_MAX_GLOW=-1\nLAG_HOT=`, "truncated-brakes.ini");
  assert.equal(malformed.frontMaxGlow, 0);
  assert.equal(malformed.rearMaxGlow, 0);
  assert.ok(malformed.warnings.some((warning) => /FRONT_MAX_GLOW must be finite/.test(warning)));
  assert.ok(malformed.warnings.some((warning) => /DISC_RR is missing/.test(warning)));
  assert.ok(malformed.warnings.some((warning) => /LAG_HOT is missing/.test(warning)));
  assert.match(parseBrakeDiscConfig("[DISCS_GRAPHICS", "truncated-header.ini").warnings.join("\n"), /DISCS_GRAPHICS section is missing/);
});

test("matches the recovered runtime target and hot-cool lag", () => {
  assert.equal(brakeDiscGlowTarget(10, 64), 0);
  assert.equal(brakeDiscGlowTarget(85, 64), 32);
  assert.equal(brakeDiscGlowTarget(-160, 64, 0.5), 32);
  assert.equal(brakeDiscGlowTarget(500, 256), 256);
  assert.equal(brakeDiscGlowStep(10, 20, 0.1, 2, 0.5), 12);
  assert.equal(brakeDiscGlowStep(20, 10, 0.1, 2, 0.5), 19.5);
  assert.equal(brakeDiscGlowStep(1, 4, 1, 2, 0.5), 4);
});

test("matches the stock blur, normal, and base-multiplied glow operations", () => {
  assert.deepEqual(stockBrakeDiscTexel([0.2, 0.4, 0.6, 0.8], [0.8, 0.6, 0.4, 0.2], 0.5), [0.5, 0.5, 0.5, 0.5]);
  const normal = stockBrakeDiscNormal([0.5, 0.5, 1], [1, 0.5, 0.5], [1, 0, 0], [0, 1, 0], [0, 0, 1], 0.5);
  assert.deepEqual(normal, [0.5, 0, 0.5]);
  assert.ok(Math.abs(Math.hypot(...normal) - Math.SQRT1_2) < 1e-12);
  assert.deepEqual(stockBrakeDiscGlow([0.25, 0.5, 1], [1, 0.5, 0.25], 64), [16, 16, 16]);
});

test("maps configured and conventional brake-disc wheel nodes", () => {
  const config = { nodes: { LF: "LEFT_FRONT_DISC", RR: "RIGHT_REAR_DISC" } };
  assert.deepEqual(resolveBrakeDiscWheel(["ROOT", "LEFT_FRONT_DISC", "mesh"], config), { corner: "LF", axle: "front", source: "brakes.ini" });
  assert.deepEqual(resolveBrakeDiscWheel(["ROOT", "GEO_discRR"], config), { corner: "RR", axle: "rear", source: "node-name" });
  assert.deepEqual(resolveBrakeDiscWheel(["ROOT", "mesh"], config), { corner: null, axle: null, source: "unmapped" });
});

test("audits exact brake-disc resources without accepting tyre slots", () => {
  const audit = auditStockBrakeDiscMaterials([
    { name: "Discs", shader: "ksBrakeDisc", resources: ["txDiffuse", "txNormal", "txGlow", "txBlur", "txNormalBlur"].map((slot) => ({ slot })) },
    { name: "Broken discs", shader: "KSBRAKEDISC", resources: [{ slot: "txDiffuse" }] },
    { name: "Tyres", shader: "ksTyres", resources: [{ slot: "txBlur" }] }
  ]);
  assert.equal(isStockBrakeDiscShader(" ksBrakeDisc "), true);
  assert.equal(audit.materials, 2);
  assert.equal(audit.completeMaterials, 1);
  assert.deepEqual(audit.entries[1].missingResources, ["txNormal", "txGlow", "txBlur", "txNormalBlur"]);
});

test("rejects malformed brake-disc state and vector input", () => {
  for (const value of [-0.01, 1.01, Infinity, NaN, "no", "", null]) assert.throws(() => normalizeBrakeDiscBlur(value));
  for (const value of [-1, Infinity, NaN, "no", "", null]) assert.throws(() => brakeDiscGlowTarget(100, value));
  assert.throws(() => stockBrakeDiscTexel([1, 1, 1], [1, 1, 1, 1], 0), /4 components/);
  assert.throws(() => stockBrakeDiscGlow("111", [1, 1, 1], 1), /3 components/);
  assert.throws(() => stockBrakeDiscNormal([0.5, 0.5, 1], [0.5, 0.5, 1], [0, 0, 0], [0, 1, 0], [0, 0, 1], 0), /zero length/);
});

test("audits the installed Porsche stock brake-disc material and missing packed configuration", async (context) => {
  let bytes, packed;
  try {
    [bytes, packed] = await Promise.all([
      readFile(assettoPath("content/cars/ks_porsche_917_30/porsche_917_30.kn5")),
      readFile(assettoPath("content/cars/ks_porsche_917_30/data.acd"))
    ]);
  } catch {
    context.skip("Installed Porsche brake-disc fixtures are unavailable");
    return;
  }
  const model = parseKn5(bytes), audit = auditStockBrakeDiscMaterials(model.materials), materialId = audit.entries[0]?.materialId;
  const meshNames = walkNodes(model.root).filter(({ node }) => (node.kind === "mesh" || node.kind === "skinnedMesh") && node.materialId === materialId).map(({ node }) => node.name).sort();
  const archive = parseAcd(packed, "ks_porsche_917_30"), entry = findAcdEntry(archive, "brakes.ini"), config = parseBrakeDiscConfig(new TextDecoder().decode(entry.data), "data.acd:brakes.ini");
  assert.equal(audit.materials, 1);
  assert.equal(audit.completeMaterials, 1);
  assert.deepEqual(meshNames, ["LOD_A_DISC_LF", "LOD_A_DISC_LR", "LOD_A_DISC_RF", "LOD_A_DISC_RR"]);
  assert.equal(config.configured, false);
  assert.match(config.warnings.join("\n"), /DISCS_GRAPHICS section is missing/);
});
