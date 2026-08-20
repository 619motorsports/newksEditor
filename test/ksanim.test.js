import assert from "node:assert/strict";
import { readFile } from "node:fs/promises";
import { join } from "node:path";
import test from "node:test";
import { ksAnimationMatrix, parseKsAnimation, sampleKsAnimationTrack, serializeKsAnimation } from "../src/ksanim.js";
import { assettoPath, carFixtureRoot } from "./fixture-paths.js";

const fixtureAnimations = join(carFixtureRoot, "animations");
const legacyAnimation = assettoPath("content/cars/619_gen6_fusion13_nsc/animations/gascap.ksanim");

function version2(tracks) {
  const chunks = [], u32 = (value) => { const bytes = Buffer.alloc(4); bytes.writeUInt32LE(value); chunks.push(bytes); }, f32 = (value) => { const bytes = Buffer.alloc(4); bytes.writeFloatLE(value); chunks.push(bytes); };
  u32(2); u32(tracks.length);
  for (const track of tracks) {
    const name = Buffer.from(track.name); u32(name.length); chunks.push(name); u32(track.frames.length);
    for (const frame of track.frames) for (const value of [...frame.quaternion, ...frame.position, ...frame.scale]) f32(value);
  }
  return Buffer.concat(chunks);
}

const frame = (position, quaternion = [0, 0, 0, 1], scale = [1, 1, 1]) => ({ quaternion, position, scale });

test("parses version 2 quaternion, position, and scale tracks", () => {
  const animation = parseKsAnimation(version2([{ name: "DOOR_L", frames: [frame([0, 0, 0]), frame([1, 2, 3], [0, 0, 1, 0], [2, 3, 4])] }]));
  assert.equal(animation.version, 2);
  assert.equal(animation.tracks[0].animated, true);
  assert.deepEqual(animation.tracks[0].frames[1], { quaternion: [0, 0, 1, 0], position: [1, 2, 3], scale: [2, 3, 4] });
  assert.equal(animation.bytesRead, animation.byteLength);
});

test("uses the game's byte-exact v2 animated-track test", () => {
  const animation = parseKsAnimation(version2([{ name: "SIGNED_ZERO", frames: [frame([0, 0, 0]), frame([-0, 0, 0])] }]));
  assert.equal(animation.tracks[0].animated, true);
  assert.ok(Object.is(animation.tracks[0].frames[1].position[0], -0));
});

test("serializes the native version 2 quatpos layout", () => {
  const expected = version2([{ name: "DÖÖR_L", frames: [frame([0, 0, 0]), frame([1, 2, 3], [0, 0, 1, 0], [2, 3, 4])] }]);
  const bytes = serializeKsAnimation({ tracks: [{ name: "DÖÖR_L", frames: [frame([0, 0, 0]), frame([1, 2, 3], [0, 0, 1, 0], [2, 3, 4])] }] });
  assert.deepEqual(Buffer.from(bytes), expected);
  const parsed = parseKsAnimation(bytes);
  assert.equal(parsed.version, 2);
  assert.equal(parsed.tracks[0].name, "DÖÖR_L");
  assert.deepEqual(parsed.tracks[0].frames[1].position, [1, 2, 3]);
});

test("rejects invalid version 2 export values", () => {
  assert.throws(() => serializeKsAnimation({}), /tracks must be an array/);
  assert.throws(() => serializeKsAnimation({ tracks: [{ name: "", frames: [] }] }), /has no name/);
  assert.throws(() => serializeKsAnimation({ tracks: [{ name: "NODE", frames: [frame([NaN, 0, 0])] }] }), /non-finite position/);
});

test("samples with the game's frameCount times normalized-position rule", () => {
  const track = { frames: [frame([0, 0, 0]), frame([10, 0, 0]), frame([20, 0, 0])] };
  assert.equal(sampleKsAnimationTrack(track, 1 / 6)[12], 5);
  assert.equal(sampleKsAnimationTrack(track, 0.5)[12], 15);
  assert.equal(sampleKsAnimationTrack(track, 2 / 3)[12], 20);
  assert.equal(sampleKsAnimationTrack(track, 1)[12], 20);
});

test("slerps DirectX row-major quaternions and linearly blends scale", () => {
  const track = { frames: [frame([0, 0, 0]), frame([0, 0, 0], [0, 0, 1, 0], [3, 1, 1])] };
  const matrix = sampleKsAnimationTrack(track, 0.25);
  assert.ok(Math.abs(matrix[0]) < 1e-6);
  assert.ok(Math.abs(matrix[1] - 2) < 1e-6);
  assert.ok(Math.abs(matrix[4] + 1) < 1e-6);
});

test("round-trips a decomposable version 1 matrix through quatpos", async (t) => {
  let bytes;
  try { bytes = await readFile(legacyAnimation); }
  catch { t.skip("Legacy KSANIM fixture is not installed"); return; }
  const animation = parseKsAnimation(bytes, "gascap.ksanim"), first = animation.tracks[0].frames[0];
  assert.equal(animation.version, 1);
  assert.equal(animation.tracks.length, 2);
  assert.equal(animation.frameCount, 4);
  assert.equal(animation.bytesRead, animation.byteLength);
  assert.ok(ksAnimationMatrix(first).every(Number.isFinite));
});

test("parses complete repository steering and gas-cap animations", async () => {
  const [steerBytes, gascapBytes] = await Promise.all([readFile(join(fixtureAnimations,"steer.ksanim")), readFile(join(fixtureAnimations,"gascap.ksanim"))]);
  const steer = parseKsAnimation(steerBytes, "steer.ksanim"), gascap = parseKsAnimation(gascapBytes, "gascap.ksanim");
  assert.deepEqual([steer.version, steer.tracks.length, steer.frameCount, steer.bytesRead], [2, 60, 100, steer.byteLength]);
  assert.deepEqual([gascap.version, gascap.tracks.length, gascap.frameCount, gascap.bytesRead], [2, 5, 100, gascap.byteLength]);
  assert.ok(steer.tracks.some((track) => track.animated));
  assert.ok(gascap.tracks.some((track) => track.animated));
});

test("rejects unsupported and truncated animation files with offsets", () => {
  assert.throws(() => parseKsAnimation(Uint8Array.from([3, 0, 0, 0, 0, 0, 0, 0])), /Unsupported KSANIM version 3.*0x0/);
  assert.throws(() => parseKsAnimation(Uint8Array.from([2, 0, 0, 0, 1, 0, 0, 0])), /Unexpected end.*track name length/);
});
