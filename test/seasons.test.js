import test from "node:test";
import assert from "node:assert/strict";
import { readdir, readFile } from "node:fs/promises";
import { adjustCspSeasonColor, analyzeCspSeasonalOverrides } from "../src/seasons.js";
import { parseCspIni } from "../src/csp-config.js";
import { assettoPath } from "./fixture-paths.js";

test("reproduces CSP autumn and winter diffuse transforms", () => {
  assert.deepEqual(adjustCspSeasonColor([0.2, 0.6, 0.1], [0, 1, 0], 0.5, 0, 0), [0.2, 0.6, 0.1]);
  close(adjustCspSeasonColor([0.2, 0.6, 0.1], [0, 1, 0], 0.5, 0.65, 0), [0.72, 0.6455, 0.035]);
  close(adjustCspSeasonColor([0.2, 0.6, 0.1], [0, 1, 0], 0.5, 0, 0.8), [0.74893704, 0.814062, 0.87918696]);
});

test("uses CSP green and upward-normal masking", () => {
  assert.deepEqual(adjustCspSeasonColor([0.5, 0.5, 0.5], [0, 0, 0], 0.5, 1, 0), [0.5, 0.5, 0.5]);
  assert.deepEqual(adjustCspSeasonColor([0.5, 0.5, 0.5], [0, 0, 0], 0.5, 0, 1), [0.5, 0.5, 0.5]);
  close(adjustCspSeasonColor([0.5, 0.5, 0.5], [0, 1, 0], 0.5, 0, 1), [0.85, 0.918, 0.986]);
});

test("reports active shader inputs separately from the legacy summer no-op", () => {
  const properties = new Map([["seasonautumn", 0.4], ["seasonwinter", 0.8], ["seasonsummer", 0.3]]);
  const status = analyzeCspSeasonalOverrides({ nodeOverrides: new Map([[{}, { properties }]]) });
  assert.deepEqual(status, { affectedMeshes: 1, autumnMeshes: 1, winterMeshes: 1, legacySummerMeshes: 1, peakAutumn: 0.4, peakWinter: 0.8, peakSummer: 0.3 });
});

test("inventories every installed loaded-track seasonal assignment", async (t) => {
  const root = assettoPath("extension/config/tracks/loaded");
  let files;
  try { files = (await readdir(root)).filter((name) => name.endsWith(".ini")); }
  catch { t.skip("Installed CSP track configs are unavailable"); return; }
  const counts = { seasonwinter: 0, seasonautumn: 0, seasonsummer: 0 };
  const affected = new Set();
  for (const file of files) {
    const config = parseCspIni(await readFile(`${root}/${file}`, "utf8"), file);
    for (const section of config.sections) {
      if (!/^MATERIAL_ADJUSTMENT/i.test(section.name)) continue;
      for (const entry of section.entries) {
        const name = entry.value.trim().toLowerCase();
        if (/^KEY_/.test(entry.key) && Object.hasOwn(counts, name)) { counts[name]++; affected.add(file); }
      }
    }
  }
  assert.deepEqual(counts, { seasonwinter: 80, seasonautumn: 66, seasonsummer: 19 });
  assert.equal(affected.size, 20);
});

function close(actual, expected) {
  assert.equal(actual.length, expected.length);
  actual.forEach((value, index) => assert.ok(Math.abs(value - expected[index]) < 1e-6, `${index}: ${value} != ${expected[index]}`));
}
