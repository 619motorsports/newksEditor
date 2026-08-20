import assert from "node:assert/strict";
import { readFile } from "node:fs/promises";
import test from "node:test";
import { parseKn5 } from "../src/kn5.js";
import { auditStockRimNodes, nativeRimBlurToggle, rimPreviewBranchActive, stockRimNodeRole, stockRimRoleForPath } from "../src/rim-blur.js";
import { assettoPath } from "./fixture-paths.js";

function node(name, active = true, children = []) { return { name, active, children }; }

test("audits exact rim names in the native wheel order", () => {
  const root = node("ROOT", true, [node("RIM_RR"), node("rim_lf"), node("RIM_BLUR_RF", false), node("RIM_LF"), node("RIM_BLUR_LF", false)]);
  const audit = auditStockRimNodes(root);
  assert.deepEqual(audit.regularNodes.map((entry) => entry.corner), ["LF", "RR"]);
  assert.deepEqual(audit.blurredNodes.map((entry) => entry.corner), ["LF", "RF"]);
  assert.equal(audit.firstRegular.node.name, "RIM_LF");
  assert.deepEqual(stockRimNodeRole("RIM_BLUR_LR"), { role: "blurred", corner: "LR" });
  assert.equal(stockRimNodeRole("rim_blur_lr"), null);
});

test("uses the first regular node for each native F1 edge", () => {
  const audit = auditStockRimNodes(node("ROOT", true, [node("RIM_LF", true), node("RIM_RF", false), node("RIM_BLUR_LF", false)]));
  assert.equal(nativeRimBlurToggle(audit), true);
  assert.equal(nativeRimBlurToggle(audit, true), false);
  assert.equal(nativeRimBlurToggle(audit, false), true);
  assert.equal(nativeRimBlurToggle(auditStockRimNodes(node("ROOT"))), null);
});

test("overrides only exact rim roots and keeps other inactive ancestors", () => {
  const root = node("ROOT"), blurred = node("RIM_BLUR_LF", false), regular = node("RIM_LF", true), mesh = node("MESH");
  assert.equal(rimPreviewBranchActive([root, blurred, mesh], true), true);
  assert.equal(rimPreviewBranchActive([root, blurred, mesh], false), false);
  assert.equal(rimPreviewBranchActive([root, regular, mesh], true), false);
  assert.equal(rimPreviewBranchActive([root, regular, mesh], false), true);
  assert.equal(rimPreviewBranchActive([node("INACTIVE", false), blurred, mesh], true), false);
  assert.deepEqual(stockRimRoleForPath([root, blurred, mesh]), { role: "blurred", corner: "LF" });
});

test("diagnoses missing and mixed rim states", () => {
  const audit = auditStockRimNodes(node("ROOT", true, [node("RIM_LF", true), node("RIM_RF", false), node("RIM_BLUR_LF", false), node("RIM_BLUR_RF", true)]));
  assert.deepEqual(audit.pairedCorners, ["LF", "RF"]);
  assert.deepEqual(audit.missingRegular, ["LR", "RR"]);
  assert.deepEqual(audit.missingBlurred, ["LR", "RR"]);
  assert.ok(audit.warnings.some((warning) => warning.includes("mixed authored active states")));
});

test("audits the installed Porsche regular and blurred rim roots", async (context) => {
  let bytes;
  try { bytes = await readFile(assettoPath("content/cars/ks_porsche_917_30/porsche_917_30.kn5")); }
  catch { context.skip("The installed Porsche rim fixture is unavailable"); return; }
  const audit = auditStockRimNodes(parseKn5(bytes).root);
  assert.deepEqual(audit.regularNodes.map((entry) => [entry.corner, entry.node.active]), [["LF", true], ["RF", true], ["LR", true], ["RR", true]]);
  assert.deepEqual(audit.blurredNodes.map((entry) => [entry.corner, entry.node.active]), [["LF", false], ["RF", false], ["LR", false], ["RR", false]]);
  assert.deepEqual(audit.pairedCorners, ["LF", "RF", "LR", "RR"]);
  assert.deepEqual(audit.warnings, []);
});
