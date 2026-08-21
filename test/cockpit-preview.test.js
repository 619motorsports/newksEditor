import assert from "node:assert/strict";
import { readFile } from "node:fs/promises";
import test from "node:test";
import { auditStockCockpitNodes, cockpitPreviewBranchActive, nativeCockpitToggle, stockCockpitNodeRole, stockCockpitRoleForPath } from "../src/cockpit-preview.js";
import { parseKn5 } from "../src/kn5.js";
import { assettoPath } from "./fixture-paths.js";

function node(name, active = true, children = []) { return { name, active, children }; }

test("audits only exact native cockpit roots", () => {
  const high = node("COCKPIT_HR", true), low = node("COCKPIT_LR", false);
  const audit = auditStockCockpitNodes(node("ROOT", true, [node("cockpit_hr"), low, high]));
  assert.equal(audit.high, high);
  assert.equal(audit.low, low);
  assert.equal(audit.available, true);
  assert.equal(stockCockpitNodeRole("COCKPIT_HR"), "high");
  assert.equal(stockCockpitNodeRole("cockpit_hr"), null);
});

test("uses the high-resolution root for each native F3 edge", () => {
  const audit = auditStockCockpitNodes(node("ROOT", true, [node("COCKPIT_HR", true), node("COCKPIT_LR", false)]));
  assert.equal(nativeCockpitToggle(audit), false);
  assert.equal(nativeCockpitToggle(audit, false), true);
  assert.equal(nativeCockpitToggle(audit, true), false);
  assert.equal(nativeCockpitToggle(auditStockCockpitNodes(node("ROOT", true, [node("COCKPIT_HR")]))), null);
});

test("overrides only cockpit roots and keeps other inactive ancestors", () => {
  const root = node("ROOT"), high = node("COCKPIT_HR", true), low = node("COCKPIT_LR", false), mesh = node("MESH");
  const audit = auditStockCockpitNodes(node("SCENE", true, [high, low]));
  assert.equal(cockpitPreviewBranchActive([root, high, mesh], true, audit), true);
  assert.equal(cockpitPreviewBranchActive([root, high, mesh], false, audit), false);
  assert.equal(cockpitPreviewBranchActive([root, low, mesh], true, audit), false);
  assert.equal(cockpitPreviewBranchActive([root, low, mesh], false, audit), true);
  assert.equal(cockpitPreviewBranchActive([node("INACTIVE", false), low, mesh], false, audit), false);
  assert.equal(stockCockpitRoleForPath([root, low, mesh]), "low");
});

test("leaves duplicate cockpit names in their authored state", () => {
  const firstHigh = node("COCKPIT_HR", true), duplicateHigh = node("COCKPIT_HR", false), low = node("COCKPIT_LR", false), root = node("ROOT");
  const audit = auditStockCockpitNodes(node("SCENE", true, [firstHigh, duplicateHigh, low]));
  assert.equal(audit.high, firstHigh);
  assert.equal(cockpitPreviewBranchActive([root, firstHigh], false, audit), false);
  assert.equal(cockpitPreviewBranchActive([root, duplicateHigh], true, audit), false);
  assert.equal(cockpitPreviewBranchActive([root, duplicateHigh], false, audit), false);
});

test("diagnoses missing, duplicate, and nonexclusive cockpit roots", () => {
  const missing = auditStockCockpitNodes(node("ROOT", true, [node("COCKPIT_HR")]));
  assert.equal(missing.available, false);
  assert.ok(missing.warnings[0].includes("COCKPIT_LR is missing"));
  const duplicate = auditStockCockpitNodes(node("ROOT", true, [node("COCKPIT_HR"), node("COCKPIT_HR"), node("COCKPIT_LR")]));
  assert.ok(duplicate.warnings.some((warning) => warning.includes("2 exact COCKPIT_HR")));
  assert.ok(duplicate.warnings.some((warning) => warning.includes("mutually exclusive")));
});

test("audits the installed Abarth cockpit roots", async (context) => {
  let bytes;
  try { bytes = await readFile(assettoPath("content/cars/abarth500/abarth500.kn5")); }
  catch { context.skip("The installed Abarth cockpit fixture is unavailable"); return; }
  const audit = auditStockCockpitNodes(parseKn5(bytes).root);
  assert.equal(audit.available, true);
  assert.equal(audit.high.active, true);
  assert.equal(audit.low.active, false);
  assert.deepEqual(audit.warnings, []);
});
