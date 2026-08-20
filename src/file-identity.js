function identityName(value) {
  if (typeof value !== "string") return "";
  return value.trim().replaceAll("\\", "/").replace(/\/{2,}/g, "/").slice(0, 2048);
}

function inputBytes(input) {
  if (input instanceof ArrayBuffer) return new Uint8Array(input.slice(0));
  if (ArrayBuffer.isView(input)) return Uint8Array.from(new Uint8Array(input.buffer, input.byteOffset, input.byteLength));
  throw new TypeError("File identity input must be binary data");
}

export function normalizeFileIdentity(value) {
  if (!value || typeof value !== "object" || Array.isArray(value)) return null;
  const name = identityName(value.name), size = Number(value.size), sha256 = typeof value.sha256 === "string" ? value.sha256.toLowerCase() : "";
  if (!name || !Number.isSafeInteger(size) || size < 0 || !/^[0-9a-f]{64}$/.test(sha256)) return null;
  const output = { name, size, sha256 };
  if (value.kn5Version !== undefined) {
    const kn5Version = Number(value.kn5Version);
    if (!Number.isSafeInteger(kn5Version) || kn5Version < 0) return null;
    output.kn5Version = kn5Version;
  }
  return output;
}

export async function createFileIdentity(name, input, metadata = {}) {
  const bytes = inputBytes(input), subtle = globalThis.crypto?.subtle;
  if (!subtle) throw new Error("SHA-256 is unavailable in this runtime");
  const digest = new Uint8Array(await subtle.digest("SHA-256", bytes));
  const identity = normalizeFileIdentity({
    name,
    size: bytes.byteLength,
    sha256: [...digest].map((value) => value.toString(16).padStart(2, "0")).join(""),
    ...(metadata.kn5Version === undefined ? {} : { kn5Version: metadata.kn5Version })
  });
  if (!identity) throw new TypeError("Could not create a valid file identity");
  return identity;
}

export function fileIdentityMatches(expected, actual) {
  const left = normalizeFileIdentity(expected), right = normalizeFileIdentity(actual);
  if (!left || !right) return false;
  if (left.name.toLowerCase() !== right.name.toLowerCase() || left.size !== right.size || left.sha256 !== right.sha256) return false;
  return left.kn5Version === undefined || right.kn5Version === undefined || left.kn5Version === right.kn5Version;
}
