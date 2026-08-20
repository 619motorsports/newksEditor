import assert from "node:assert/strict";
import test from "node:test";
import { parseModelsIni, serializeModelsIni } from "../src/kn5-workspace.js";
import { applyWorkspaceEdits, captureWorkspaceBaseline, workspaceEditCount } from "../src/workspace-authoring.js";

const identity = () => [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1];

function model() {
  const dynamic = { index: 2, probability: 75, multiplicity: [1, 3], posMode: "RANDOM", positionCenter: [10, 20, 30], positionRange: [4, 5, 6], velMode: "RANDOM", velocityBase: [1, 2, 3], velocityRange: [7, 8, 9], playWav: "fly.wav" };
  return {
    root: { children: [{ transform: identity() }, { transform: identity() }] },
    workspace: {
      cockpitHrDistance: null,
      driverHrDistance: null,
      files: [
        { name: "main.kn5", position: [0, 0, 0], rotation: [0, 0, 0], lod: null, dynamic: null },
        { name: "plane.kn5", position: [10, 20, 30], rotation: [0, 0, 0], lod: null, dynamic }
      ]
    }
  };
}

test("applies and restores static and dynamic workspace edits", () => {
  const source = model(), baseline = captureWorkspaceBaseline(source), edits = { files: {
    "0": { position: [1, 2, 3], rotation: [90, 0, 0] },
    "1": { probability: 25, multiplicity: [2, 5], posMode: "FIXED", positionCenter: [-10, 40, 50], positionRange: [1, 2, 3], velMode: "LINEAR", velocityBase: [8, 9, 10], velocityRange: [0, 1, 2], playWav: null }
  } };
  assert.equal(workspaceEditCount(edits), 11);
  assert.equal(applyWorkspaceEdits(source, edits, baseline), 11);
  assert.deepEqual(source.workspace.files[0].position, [1, 2, 3]);
  assert.deepEqual(source.root.children[0].transform.slice(12, 15), [1, 2, 3]);
  const dynamic = source.workspace.files[1].dynamic;
  assert.deepEqual(dynamic, { index: 2, probability: 25, multiplicity: [2, 5], posMode: "FIXED", positionCenter: [-10, 40, 50], positionRange: [1, 2, 3], velMode: "LINEAR", velocityBase: [8, 9, 10], velocityRange: [0, 1, 2], playWav: "" });
  assert.deepEqual(source.workspace.files[1].position, [-10, 40, 50]);
  assert.deepEqual(source.root.children[1].transform.slice(12, 15), [-10, 40, 50]);
  assert.equal(applyWorkspaceEdits(source, {}, baseline), 0);
  assert.deepEqual(source.workspace.files[0].position, [0, 0, 0]);
  assert.deepEqual(source.workspace.files[1].dynamic, { index: 2, probability: 75, multiplicity: [1, 3], posMode: "RANDOM", positionCenter: [10, 20, 30], positionRange: [4, 5, 6], velMode: "RANDOM", velocityBase: [1, 2, 3], velocityRange: [7, 8, 9], playWav: "fly.wav" });
  assert.deepEqual(source.root.children[1].transform.slice(12, 15), [10, 20, 30]);
});

test("round-trips authored dynamic fields through models.ini", () => {
  const source = model(), baseline = captureWorkspaceBaseline(source);
  applyWorkspaceEdits(source, { files: { "1": {
    probability: 25,
    multiplicity: [2, 5],
    posMode: "FIXED",
    positionCenter: [-10, 40, 50],
    positionRange: [1, 2, 3],
    velMode: "LINEAR",
    velocityBase: [8, 9, 10],
    velocityRange: [0, 1, 2],
    playWav: null
  } } }, baseline);

  const manifest = serializeModelsIni(source.workspace);
  assert.doesNotMatch(manifest, /PLAY_WAV/);
  const parsed = parseModelsIni(manifest, "exported-models.ini");
  assert.deepEqual(parsed.warnings, []);
  assert.deepEqual(parsed.dynamicObjects[0], {
    index: 2,
    file: "plane.kn5",
    probability: 25,
    multiplicity: [2, 5],
    posMode: "FIXED",
    positionCenter: [-10, 40, 50],
    positionRange: [1, 2, 3],
    velMode: "LINEAR",
    velocityBase: [8, 9, 10],
    velocityRange: [0, 1, 2],
    playWav: "",
    section: "DYNAMIC_OBJECT_2",
    line: 6
  });
});
