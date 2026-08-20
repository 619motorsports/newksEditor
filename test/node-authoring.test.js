import assert from "node:assert/strict";
import test from "node:test";
import { applyNodeEdits, composeNodeTransform, decomposeNodeTransform, nodeAtPath, nodePathEntries } from "../src/node-authoring.js";

function tree() {
  return { kind: "node", name: "root", active: true, children: [
    { kind: "node", name: "A", active: true, children: [] },
    { kind: "node", name: "B", active: true, children: [{ kind: "mesh", name: "A", active: true, children: [] }] }
  ] };
}

test("addresses duplicate names with stable root-relative paths", () => {
  const root = tree();
  assert.deepEqual(nodePathEntries(root).map(({ path }) => path), ["root", "0", "1", "1/0"]);
  assert.equal(nodeAtPath(root, "1/0").kind, "mesh");
  assert.equal(nodeAtPath(root, "2"), null);
  assert.equal(applyNodeEdits(root, { "1": { name: "RENAMED", active: false } }), 1);
  assert.equal(root.children[0].name, "A");
  assert.equal(root.children[1].name, "RENAMED");
  assert.equal(root.children[1].active, false);
});

test("round-trips editable position, Euler rotation, and scale", () => {
  const transform = composeNodeTransform({ position: [2, -3, 4], rotation: [20, -35, 70], scale: [1.5, 2, 0.75] });
  const result = decomposeNodeTransform(transform);
  assert.equal(result.decomposable, true);
  assert.deepEqual(result.position.map((value) => Math.round(value)), [2, -3, 4]);
  assert.ok(Math.max(...result.rotation.map((value, index) => Math.abs(value - [20, -35, 70][index]))) < 1e-9);
  assert.ok(Math.max(...result.scale.map((value, index) => Math.abs(value - [1.5, 2, 0.75][index]))) < 1e-9);
});

test("warns instead of assigning transforms to mesh nodes", () => {
  const root = tree(), warnings = [];
  assert.equal(applyNodeEdits(root, { "1/0": { transform: Array(16).fill(0) } }, warnings), 0);
  assert.match(warnings[0], /cannot store a local transform/);
});
