const utf8 = new TextDecoder("utf-8");

const MAX_ACD_ENTRIES = 65_536;
const MAX_ACD_NAME_BYTES = 4_096;
const MAX_ACD_ENTRY_BYTES = 64 * 1024 * 1024;
const MAX_ACD_CONTAINER_BYTES = 512 * 1024 * 1024;

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

function toBytes(input, label = "data.acd input") {
  if (input instanceof Uint8Array) return input;
  if (input instanceof ArrayBuffer) return new Uint8Array(input);
  if (ArrayBuffer.isView(input)) return new Uint8Array(input.buffer, input.byteOffset, input.byteLength);
  throw new TypeError(`${label} must be an ArrayBuffer or typed array`);
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

function writeEncryptedAcdPayload(output, offset, data, key) {
  const view = new DataView(output.buffer, output.byteOffset, output.byteLength);
  for (let index = 0; index < data.byteLength; index++) view.setInt32(offset + index * 4, data[index] + key.charCodeAt(index % key.length), true);
}

export function encryptAcdPayload(input, password) {
  const data = toBytes(input, "ACD replacement data"), key = String(password);
  if (!key) throw new TypeError("An ACD password is required");
  if (data.byteLength > MAX_ACD_ENTRY_BYTES) throw new RangeError(`ACD replacement data cannot exceed ${MAX_ACD_ENTRY_BYTES} bytes`);
  const stored = new Uint8Array(data.byteLength * 4);
  writeEncryptedAcdPayload(stored, 0, data, key);
  return stored;
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
    const data = bytes.subarray(offset, offset + length), value = utf8.decode(data); offset += length; return { value, data };
  };

  if (bytes.byteLength >= 4 && view.getInt32(0, true) === -1111) {
    offset = 4;
    header = readInt("ACD header value");
  }

  while (offset < bytes.byteLength) {
    const recordOffset = offset, decodedName = readString(), name = decodedName.value, lengthOffset = offset, length = readInt("entry payload length");
    if (length < 0) throw new AcdError(`Negative payload length for ${name || "entry"}`, lengthOffset);
    const storedLength = length * 4;
    if (!Number.isSafeInteger(storedLength)) throw new AcdError(`Payload length is too large for ${name || "entry"}`, lengthOffset);
    need(storedLength, `payload for ${name || "entry"}`);
    const payloadOffset = offset, storedData = bytes.subarray(offset, offset + storedLength), data = decryptAcdPayload(storedData, length, key.password);
    offset += storedLength;
    const normalized = safePath(name);
    if (!normalized.safe) warnings.push(`${source}: ignored unsafe archive path ${JSON.stringify(name)}`);
    const entry = { name, nameBytes: decodedName.data, path: normalized.path, safe: normalized.safe, data, storedData, size: length, storedBytes: storedLength, recordOffset, payloadOffset };
    entries.push(entry);
    if (normalized.safe) {
      const lookup = normalized.path.toLowerCase();
      if (byPath.has(lookup)) warnings.push(`${source}: duplicate archive path ${normalized.path}`);
      else byPath.set(lookup, entry);
    }
  }

  return { source, assetName: key.assetName, key: key.password, keyOctets: key.octets, header, entries, byPath, warnings, byteLength: bytes.byteLength, bytesRead: offset, sourceBytes: bytes };
}

function replacementMap(replacements) {
  const output = new Map(), values = replacements instanceof Map ? replacements : Object.entries(replacements || {});
  for (const [name, input] of values) {
    const normalized = safePath(String(name || ""));
    if (!normalized.safe) throw new TypeError(`Unsafe ACD replacement path ${JSON.stringify(name)}`);
    const key = normalized.path.toLowerCase();
    if (output.has(key)) throw new TypeError(`Duplicate ACD replacement path ${normalized.path}`);
    output.set(key, { path: normalized.path, data: toBytes(input, `Replacement for ${normalized.path}`) });
  }
  return output;
}

/** Rebuild a parsed data.acd while preserving every untouched name and payload byte. */
export function serializeAcd(archive, replacements = new Map()) {
  if (!archive || !Array.isArray(archive.entries)) throw new TypeError("A parsed data.acd archive is required");
  if (archive.entries.length > MAX_ACD_ENTRIES) throw new RangeError(`data.acd cannot contain more than ${MAX_ACD_ENTRIES} entries`);
  const replacementByPath = replacementMap(replacements);
  if (!replacementByPath.size && archive.sourceBytes) return toBytes(archive.sourceBytes).slice();

  const key = createAcdKey(archive.assetName), archiveEntries = new Map();
  for (const entry of archive.entries) {
    if (!entry?.safe) throw new TypeError(`Cannot repack unsafe ACD path ${JSON.stringify(entry?.name || "")}`);
    const lookup = String(entry.path || "").toLowerCase();
    if (archiveEntries.has(lookup)) throw new TypeError(`Cannot repack duplicate ACD path ${entry.path}`);
    archiveEntries.set(lookup, entry);
  }
  for (const replacement of replacementByPath.values()) {
    if (!archiveEntries.has(replacement.path.toLowerCase())) throw new TypeError(`ACD replacement path was not found: ${replacement.path}`);
    if (replacement.data.byteLength > MAX_ACD_ENTRY_BYTES) throw new RangeError(`Replacement for ${replacement.path} cannot exceed ${MAX_ACD_ENTRY_BYTES} bytes`);
  }

  const records = [], headerBytes = archive.header === null || archive.header === undefined ? 0 : 8;
  if (headerBytes && (!Number.isInteger(archive.header) || archive.header < -0x80000000 || archive.header > 0x7fffffff)) throw new RangeError("ACD header must be a signed 32-bit integer");
  let byteLength = headerBytes;
  for (const entry of archive.entries) {
    const nameBytes = toBytes(entry.nameBytes, `ACD name bytes for ${entry.path}`);
    if (!nameBytes.byteLength || nameBytes.byteLength > MAX_ACD_NAME_BYTES) throw new RangeError(`ACD path ${entry.path} must use 1 to ${MAX_ACD_NAME_BYTES} bytes`);
    const replacement = replacementByPath.get(entry.path.toLowerCase()), data = replacement?.data || toBytes(entry.data, `ACD data for ${entry.path}`);
    if (data.byteLength > MAX_ACD_ENTRY_BYTES) throw new RangeError(`ACD entry ${entry.path} cannot exceed ${MAX_ACD_ENTRY_BYTES} bytes`);
    const stored = replacement ? null : toBytes(entry.storedData, `Stored ACD data for ${entry.path}`), storedLength = data.byteLength * 4;
    if (stored && stored.byteLength !== storedLength) throw new RangeError(`Stored ACD data for ${entry.path} has the wrong length`);
    const recordLength = 8 + nameBytes.byteLength + storedLength;
    if (!Number.isSafeInteger(byteLength + recordLength) || byteLength + recordLength > MAX_ACD_CONTAINER_BYTES) throw new RangeError(`data.acd cannot exceed ${MAX_ACD_CONTAINER_BYTES} bytes`);
    byteLength += recordLength;
    records.push({ nameBytes, data, stored, storedLength, replacement: Boolean(replacement) });
  }

  const output = new Uint8Array(byteLength), view = new DataView(output.buffer); let offset = 0;
  const writeInt = (value) => { view.setInt32(offset, value, true); offset += 4; };
  if (headerBytes) { writeInt(-1111); writeInt(archive.header); }
  for (const record of records) {
    writeInt(record.nameBytes.byteLength); output.set(record.nameBytes, offset); offset += record.nameBytes.byteLength;
    writeInt(record.data.byteLength);
    if (record.replacement) writeEncryptedAcdPayload(output, offset, record.data, key.password);
    else output.set(record.stored, offset);
    offset += record.storedLength;
  }
  return output;
}

export function findAcdEntry(archive, path) {
  return archive?.byPath?.get(String(path || "").replaceAll("\\", "/").toLowerCase()) || null;
}
