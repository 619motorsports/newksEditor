const utf8 = new TextDecoder("utf-8");

export class AcdError extends Error {
  constructor(message, offset = 0) {
    super(`${message} at byte ${offset}`);
    this.name = "AcdError";
    this.offset = offset;
  }
}

function int32(value) { return value | 0; }
function multiply(a, b) { return Math.imul(a, b); }
function byte(value) { return ((value % 256) + 256) % 256; }

// Assetto Corsa derives an eight-number password from the lower-case asset
// directory, then cycles over the ASCII characters of that decimal string.
export function createAcdKey(assetName) {
  const name = String(assetName || "").trim().toLowerCase();
  if (!name) throw new TypeError("An asset directory name is required for data.acd");
  const codes = [...name].map((character) => character.charCodeAt(0));

  let a = 0;
  for (const code of codes) a = int32(a + code);

  let b = 0;
  for (let index = 0; index < codes.length - 1; index += 2) b = int32(multiply(b, codes[index]) - codes[index + 1]);

  let c = 0;
  for (let index = 1; index < codes.length - 3; index += 3) {
    c = int32(Math.trunc(multiply(c, codes[index]) / (codes[index + 1] + 27)) - 27 - codes[index - 1]);
  }

  let d = 5763;
  for (let index = 1; index < codes.length; index++) d = int32(d - codes[index]);

  let e = 66;
  for (let index = 1; index < codes.length - 4; index += 4) {
    e = int32(multiply(multiply(codes[index] + 15, e), codes[index - 1] + 15) + 22);
  }

  let f = 101;
  for (let index = 0; index < codes.length - 2; index += 2) f = int32(f - codes[index]);

  let g = 171;
  for (let index = 0; index < codes.length - 2; index += 2) g %= codes[index];

  let h = 171;
  for (let index = 0; index < codes.length - 1; index++) h = int32(Math.trunc(h / codes[index]) + codes[index + 1]);

  const octets = [a, b, c, d, e, f, g, h].map(byte);
  return { assetName: name, octets, password: octets.join("-") };
}

function toBytes(input) {
  if (input instanceof Uint8Array) return input;
  if (input instanceof ArrayBuffer) return new Uint8Array(input);
  if (ArrayBuffer.isView(input)) return new Uint8Array(input.buffer, input.byteOffset, input.byteLength);
  throw new TypeError("data.acd input must be an ArrayBuffer or Uint8Array");
}

function safePath(name) {
  const path = name.replaceAll("\\", "/");
  const parts = path.split("/");
  const safe = Boolean(path) && !path.includes("\0") && !path.startsWith("/") && !/^[a-z]:\//i.test(path) && parts.every((part) => part && part !== "." && part !== "..");
  return { path, safe };
}

export function decryptAcdPayload(storedBytes, plainLength, password) {
  const stored = toBytes(storedBytes), key = String(password);
  if (!key) throw new TypeError("An ACD password is required");
  if (!Number.isSafeInteger(plainLength) || plainLength < 0 || plainLength * 4 > stored.byteLength) throw new RangeError("Invalid ACD payload length");
  const data = new Uint8Array(plainLength);
  for (let index = 0; index < plainLength; index++) data[index] = byte(stored[index * 4] - key.charCodeAt(index % key.length));
  return data;
}

export function parseAcd(input, assetName, source = "data.acd") {
  const bytes = toBytes(input), view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  const key = createAcdKey(assetName), warnings = [], entries = [], byPath = new Map();
  let offset = 0, header = null;
  const need = (count, what) => {
    if (!Number.isSafeInteger(count) || count < 0 || offset + count > bytes.byteLength) throw new AcdError(`Truncated ${what}`, offset);
  };
  const readInt = (what) => { need(4, what); const value = view.getInt32(offset, true); offset += 4; return value; };
  const readString = () => {
    const lengthOffset = offset, length = readInt("entry-name length");
    if (length < 0) throw new AcdError("Negative entry-name length", lengthOffset);
    need(length, "entry name");
    const value = utf8.decode(bytes.subarray(offset, offset + length)); offset += length; return value;
  };

  if (bytes.byteLength >= 4 && view.getInt32(0, true) === -1111) {
    offset = 4;
    header = readInt("ACD header value");
  }

  while (offset < bytes.byteLength) {
    const recordOffset = offset, name = readString(), lengthOffset = offset, length = readInt("entry payload length");
    if (length < 0) throw new AcdError(`Negative payload length for ${name || "entry"}`, lengthOffset);
    const storedLength = length * 4;
    if (!Number.isSafeInteger(storedLength)) throw new AcdError(`Payload length is too large for ${name || "entry"}`, lengthOffset);
    need(storedLength, `payload for ${name || "entry"}`);
    const payloadOffset = offset, data = decryptAcdPayload(bytes.subarray(offset, offset + storedLength), length, key.password);
    offset += storedLength;
    const normalized = safePath(name);
    if (!normalized.safe) warnings.push(`${source}: ignored unsafe archive path ${JSON.stringify(name)}`);
    const entry = { name, path: normalized.path, safe: normalized.safe, data, size: length, storedBytes: storedLength, recordOffset, payloadOffset };
    entries.push(entry);
    if (normalized.safe) {
      const lookup = normalized.path.toLowerCase();
      if (byPath.has(lookup)) warnings.push(`${source}: duplicate archive path ${normalized.path}`);
      else byPath.set(lookup, entry);
    }
  }

  return { source, assetName: key.assetName, key: key.password, keyOctets: key.octets, header, entries, byPath, warnings, byteLength: bytes.byteLength, bytesRead: offset };
}

export function findAcdEntry(archive, path) {
  return archive?.byPath?.get(String(path || "").replaceAll("\\", "/").toLowerCase()) || null;
}
