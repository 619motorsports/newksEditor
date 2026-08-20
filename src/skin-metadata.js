export const SKIN_METADATA_TEXT_FIELDS = Object.freeze(["skinname", "drivername", "country", "team", "number"]);
export const SKIN_METADATA_FIELDS = Object.freeze([...SKIN_METADATA_TEXT_FIELDS, "priority"]);
export const MAX_SKIN_METADATA_BYTES = 1024 * 1024;

const UNSAFE_KEYS = new Set(["__proto__", "constructor", "prototype"]);

export class SkinMetadataError extends Error {
  constructor(message, cause) {
    super(message, cause ? { cause } : undefined);
    this.name = "SkinMetadataError";
  }
}

/** Keep asynchronous metadata reads attached to the latest selected skin. */
export function createSkinMetadataLoadGuard() {
  let generation = 0;
  return {
    invalidate() { generation += 1; },
    start(skinName) {
      const token = ++generation, expectedName = String(skinName || "");
      return { isCurrent: (currentName) => token === generation && String(currentName || "") === expectedName };
    }
  };
}

function inputText(input, source) {
  if (typeof input === "string") {
    const bytes = new TextEncoder().encode(input);
    if (bytes.byteLength > MAX_SKIN_METADATA_BYTES) throw new SkinMetadataError(`${source} exceeds the ${MAX_SKIN_METADATA_BYTES}-byte limit`);
    return { text: input.replace(/^\uFEFF/, ""), bytes: bytes.byteLength };
  }
  let bytes;
  if (input instanceof Uint8Array) bytes = input;
  else if (input instanceof ArrayBuffer) bytes = new Uint8Array(input);
  else if (ArrayBuffer.isView(input)) bytes = new Uint8Array(input.buffer, input.byteOffset, input.byteLength);
  else throw new TypeError("Skin metadata input must be text, an ArrayBuffer, or a typed array");
  if (bytes.byteLength > MAX_SKIN_METADATA_BYTES) throw new SkinMetadataError(`${source} exceeds the ${MAX_SKIN_METADATA_BYTES}-byte limit`);
  try { return { text: new TextDecoder("utf-8", { fatal: true }).decode(bytes), bytes: bytes.byteLength }; }
  catch (error) { throw new SkinMetadataError(`${source} is not valid UTF-8`, error); }
}

function safeJsonValue(value, warnings, path = "", depth = 0) {
  if (depth > 32) throw new SkinMetadataError("Skin metadata nesting exceeds 32 levels");
  if (value === null || typeof value === "string" || typeof value === "boolean" || typeof value === "number") return value;
  if (Array.isArray(value)) return value.map((item, index) => safeJsonValue(item, warnings, `${path}[${index}]`, depth + 1));
  if (!value || typeof value !== "object") return null;
  const output = Object.create(null);
  for (const [key, item] of Object.entries(value)) {
    if (UNSAFE_KEYS.has(key)) { warnings.push(`${path || "metadata"}: ignored unsafe key ${key}`); continue; }
    output[key] = safeJsonValue(item, warnings, path ? `${path}.${key}` : key, depth + 1);
  }
  return output;
}

function textField(value, key, warnings) {
  if (value === undefined) return "";
  if (typeof value === "string") return value.slice(0, 4096);
  if (key === "number" && typeof value === "number" && Number.isFinite(value)) return String(value);
  warnings.push(`${key} must be a string${key === "number" ? " or finite number" : ""}`);
  return "";
}

function priorityField(value, warnings) {
  if (value === undefined) return null;
  if (Number.isSafeInteger(value) && value >= 0) return value;
  warnings.push("priority must be a nonnegative integer");
  return null;
}

/** Parse bounded, untrusted Assetto Corsa ui_skin.json input. */
export function parseSkinMetadata(input, source = "ui_skin.json") {
  const decoded = inputText(input, source);
  let value;
  try { value = JSON.parse(decoded.text); }
  catch (error) { throw new SkinMetadataError(`${source} is not valid JSON: ${error.message}`, error); }
  if (!value || typeof value !== "object" || Array.isArray(value)) throw new SkinMetadataError(`${source} must contain one JSON object`);
  const warnings = [], original = safeJsonValue(value, warnings);
  const metadata = Object.create(null);
  for (const key of SKIN_METADATA_TEXT_FIELDS) metadata[key] = textField(original[key], key, warnings);
  metadata.priority = priorityField(original.priority, warnings);
  return { source, byteLength: decoded.bytes, metadata, original, warnings };
}

/** Reject oversized browser files before allocating their contents. */
export async function readSkinMetadataFile(file, source = "ui_skin.json") {
  if (!file || typeof file.arrayBuffer !== "function") throw new TypeError("Skin metadata file must provide arrayBuffer()");
  if (!Number.isSafeInteger(file.size) || file.size < 0) throw new TypeError("Skin metadata file size must be a nonnegative safe integer");
  if (file.size > MAX_SKIN_METADATA_BYTES) throw new SkinMetadataError(`${source} exceeds the ${MAX_SKIN_METADATA_BYTES}-byte limit`);
  return parseSkinMetadata(await file.arrayBuffer(), source);
}

export function createSkinMetadata(source = "ui_skin.json") {
  const original = Object.create(null);
  for (const key of SKIN_METADATA_TEXT_FIELDS) original[key] = "";
  return { source, byteLength: 0, metadata: { ...original, priority: null }, original, warnings: [] };
}

export function normalizeSkinMetadataEdit(value) {
  if (!value || typeof value !== "object" || Array.isArray(value)) return null;
  const output = Object.create(null);
  for (const key of SKIN_METADATA_TEXT_FIELDS) if (typeof value[key] === "string") output[key] = value[key].slice(0, 4096);
  if (Number.isSafeInteger(value.priority) && value.priority >= 0) output.priority = value.priority;
  return Object.keys(output).length ? output : null;
}

export function effectiveSkinMetadata(parsed, edit = null) {
  const normalized = normalizeSkinMetadataEdit(edit) || {};
  return { ...parsed.metadata, ...normalized };
}

/** Serialize edited metadata while retaining safe, unknown source fields. */
export function serializeSkinMetadata(parsed, edit = null) {
  if (!parsed?.metadata || !parsed?.original) throw new TypeError("Parsed skin metadata is required");
  const metadata = effectiveSkinMetadata(parsed, edit), output = Object.create(null);
  for (const key of SKIN_METADATA_TEXT_FIELDS) output[key] = metadata[key];
  if (metadata.priority !== null) output.priority = metadata.priority;
  for (const [key, value] of Object.entries(parsed.original)) if (!SKIN_METADATA_FIELDS.includes(key) && !UNSAFE_KEYS.has(key)) output[key] = value;
  return `${JSON.stringify(output, null, 2)}\n`;
}
