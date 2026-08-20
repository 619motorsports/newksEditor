import assert from "node:assert/strict";
import test from "node:test";
import { createFileIdentity, fileIdentityMatches, normalizeFileIdentity } from "../src/file-identity.js";

test("creates stable SHA-256 identities for file bytes", async () => {
  const bytes = new Uint8Array([0x73, 0x63, 0x36, 0x39, 0x36, 0x39, 6, 0, 0, 0]);
  const first = await createFileIdentity("collider.kn5", bytes, { kn5Version: 6 });
  const second = await createFileIdentity("COLLIDER.KN5", bytes.slice(), { kn5Version: 6 });
  assert.equal(first.size, bytes.byteLength);
  assert.match(first.sha256, /^[0-9a-f]{64}$/);
  assert.equal(fileIdentityMatches(first, second), true);
});

test("rejects a replaced file with the same name and size", async () => {
  const original = await createFileIdentity("collider.kn5", new Uint8Array([1, 2, 3, 4]), { kn5Version: 6 });
  const replacement = await createFileIdentity("collider.kn5", new Uint8Array([1, 2, 3, 5]), { kn5Version: 6 });
  assert.equal(original.size, replacement.size);
  assert.equal(fileIdentityMatches(original, replacement), false);
});

test("rejects malformed persisted identities", () => {
  const valid = { name: "collider.kn5", size: 128, sha256: "a".repeat(64), kn5Version: 6 };
  assert.deepEqual(normalizeFileIdentity({ ...valid, sha256: valid.sha256.toUpperCase() }), valid);
  for (const value of [
    null,
    [],
    { ...valid, name: "" },
    { ...valid, size: -1 },
    { ...valid, size: Number.MAX_SAFE_INTEGER + 1 },
    { ...valid, sha256: "a".repeat(63) },
    { ...valid, sha256: `${"a".repeat(63)}x` },
    { ...valid, kn5Version: -1 }
  ]) assert.equal(normalizeFileIdentity(value), null);
  assert.equal(fileIdentityMatches(valid, null), false);
});

test("rejects non-binary identity input", async () => {
  await assert.rejects(() => createFileIdentity("collider.kn5", "truncated"), /binary data/);
});
