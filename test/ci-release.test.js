import assert from "node:assert/strict";
import { readFile } from "node:fs/promises";
import test from "node:test";

const workflow = await readFile(new URL("../.github/workflows/ci-release.yml", import.meta.url), "utf8");

test("does not publish packages from untagged builds", () => {
  assert.match(workflow, /- name: Build packages[\s\S]*?run: npm run \$\{\{ matrix\.script \}\} -- --publish never/);
});

test("publishes packages only from tagged builds", () => {
  assert.match(workflow, /- name: Publish tagged packages[\s\S]*?run: npm run \$\{\{ matrix\.script \}\} -- --publish always/);
});
