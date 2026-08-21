import assert from "node:assert/strict";
import { readFile } from "node:fs/promises";
import { spawnSync } from "node:child_process";
import { fileURLToPath } from "node:url";
import test from "node:test";

const tool = new URL("../tools/browser-smoke.mjs", import.meta.url);
const toolPath = fileURLToPath(tool);
const source = await readFile(tool, "utf8");

test("browser smoke advertises and parses the wireframe option before connecting", () => {
  const result = spawnSync(process.execPath, [toolPath, "--wireframe"], { encoding: "utf8" });
  assert.notEqual(result.status, 0);
  assert.match(`${result.stdout}\n${result.stderr}`, /\[--wireframe\]/);
  assert.match(source, /const wireframe = process\.argv\.includes\("--wireframe"\)/);
});

test("wireframe smoke captures solid state, toggles the production control, then clears it", () => {
  const start = source.indexOf("if (wireframe) {");
  const block = source.slice(start, source.indexOf("if(selectionAxis)", start));
  assert.match(block, /states\["wireframe=off"\] = await screenshotState\(screenshotPath\)/);
  assert.match(block, /document\.querySelector\('#wireframe'\).*\.click\(\)/);
  assert.match(block, /await waitFor\(`window\.__apexRenderer\?\.wireframe===true`\)/);
  assert.match(block, /states\["wireframe=on"\] = await screenshotState\(\)/);
  assert.ok(block.indexOf('states["wireframe=off"]') < block.indexOf('states["wireframe=on"]'));
  assert.match(block, /await waitFor\(`window\.\__apexRenderer\?\.wireframe===false`\)/);
});

test("wireframe smoke records state and rejects identical or WebGL-error captures", () => {
  assert.match(source, /wireframe: await evaluate\(`Boolean\(window\.\__apexRenderer\?\.wireframe\)`\)/);
  assert.match(source, /states\["wireframe=off"\]\.hash === states\["wireframe=on"\]\.hash/);
  assert.match(source, /const failedStates=Object\.entries\(states\)\.filter\(\(\[,state\]\)=>state\?\.glError\)/);
  assert.match(source, /if\(errors\.length\|\|failedStates\.length\)throw new Error/);
});
