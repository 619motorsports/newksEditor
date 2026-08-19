import { readdir, readFile, stat } from "node:fs/promises";
import { basename, join, resolve } from "node:path";

function option(name, fallback = "") {
  const index = process.argv.indexOf(`--${name}`);
  return index >= 0 ? process.argv[index + 1] : fallback;
}

const roots = process.argv.slice(2).filter((value, index, values) => !value.startsWith("--") && (index === 0 || !values[index - 1].startsWith("--")));
const limit = Math.max(1, Number(option("limit", "200")) || 200);
const skip = Math.max(0, Number(option("skip", "0")) || 0), collectLimit = limit + skip;
if (!roots.length) throw new Error("Usage: node tools/kn5-texture-audit.mjs PATH [PATH…] [--skip 0] [--limit 200]");

async function collect(path, files) {
  if (files.length >= collectLimit) return;
  const info = await stat(path);
  if (info.isFile()) {
    if (/\.kn5$/i.test(path)) files.push(resolve(path));
    return;
  }
  const entries = await readdir(path, { withFileTypes: true });
  entries.sort((a, b) => a.name.localeCompare(b.name));
  for (const entry of entries) {
    if (files.length >= collectLimit) break;
    if (entry.isDirectory() || entry.isFile() && /\.kn5$/i.test(entry.name)) await collect(join(path, entry.name), files);
  }
}

function readU32(buffer, offset) {
  if (offset + 4 > buffer.length) throw new Error(`Unexpected end at ${offset}`);
  return buffer.readUInt32LE(offset);
}

function readString(buffer, cursor) {
  const length = readU32(buffer, cursor.offset); cursor.offset += 4;
  if (length > 1_048_576 || cursor.offset + length > buffer.length) throw new Error(`Invalid string length ${length}`);
  const value = buffer.subarray(cursor.offset, cursor.offset + length).toString("utf8"); cursor.offset += length;
  return value;
}

const dxgi = new Map([
  [28, "RGBA8"], [29, "RGBA8_SRGB"], [61, "R8"], [71, "BC1"], [72, "BC1_SRGB"],
  [74, "BC2"], [75, "BC2_SRGB"], [77, "BC3"], [78, "BC3_SRGB"], [80, "BC4_UNORM"],
  [81, "BC4_SNORM"], [83, "BC5_UNORM"], [84, "BC5_SNORM"], [95, "BC6H_UF16"],
  [96, "BC6H_SF16"], [98, "BC7"], [99, "BC7_SRGB"], [87, "BGRA8"], [91, "BGRA8_SRGB"]
]);

function ddsFormat(data) {
  if (data.length < 128 || data.subarray(0, 4).toString("ascii") !== "DDS ") return data.subarray(0, 8).toString("hex") || "EMPTY";
  const flags = data.readUInt32LE(80), fourCC = data.subarray(84, 88).toString("ascii").replaceAll("\0", "");
  if (flags & 4) {
    if (fourCC === "DX10") return data.length >= 148 ? dxgi.get(data.readUInt32LE(128)) || `DXGI_${data.readUInt32LE(128)}` : "DX10_TRUNCATED";
    return ({ DXT1: "BC1", DXT2: "BC2_PREMULT", DXT3: "BC2", DXT4: "BC3_PREMULT", DXT5: "BC3", ATI1: "BC4_UNORM", BC4U: "BC4_UNORM", BC4S: "BC4_SNORM", ATI2: "BC5_UNORM", BC5U: "BC5_UNORM", BC5S: "BC5_SNORM" })[fourCC] || `FOURCC_${fourCC || "0"}`;
  }
  const bits = data.readUInt32LE(88), r = data.readUInt32LE(92), g = data.readUInt32LE(96), b = data.readUInt32LE(100), a = data.readUInt32LE(104);
  return `RAW_${bits}_R${r.toString(16)}_G${g.toString(16)}_B${b.toString(16)}_A${a.toString(16)}`;
}

function scanTextures(buffer) {
  if (buffer.length < 14 || buffer.subarray(0, 6).toString("ascii") !== "sc6969") throw new Error("Not a KN5 file");
  const version = readU32(buffer, 6);
  if (version !== 5 && version !== 6) throw new Error(`Unsupported KN5 version ${version}`);
  const cursor = { offset: version >= 6 ? 14 : 10 }, count = readU32(buffer, cursor.offset); cursor.offset += 4;
  if (count > 100_000) throw new Error(`Invalid texture count ${count}`);
  const textures = [];
  for (let index = 0; index < count; index++) {
    const active = Boolean(readU32(buffer, cursor.offset)); cursor.offset += 4;
    const name = readString(buffer, cursor), size = readU32(buffer, cursor.offset); cursor.offset += 4;
    if (cursor.offset + size > buffer.length) throw new Error(`Texture ${name} exceeds file`);
    const data = buffer.subarray(cursor.offset, cursor.offset + size); cursor.offset += size;
    textures.push({ active, name, size, format: ddsFormat(data) });
  }
  return textures;
}

const files = [];
for (const root of roots) await collect(resolve(root), files);
files.splice(0, skip);
const formats = new Map(), examples = new Map(), failures = [];
let textureCount = 0, textureBytes = 0;
for (const file of files) {
  try {
    for (const texture of scanTextures(await readFile(file))) {
      textureCount++; textureBytes += texture.size;
      formats.set(texture.format, (formats.get(texture.format) || 0) + 1);
      const list = examples.get(texture.format) || [];
      if (list.length < 4) list.push(`${basename(file)}:${texture.name}`);
      examples.set(texture.format, list);
    }
  } catch (error) { failures.push({ file, error: error.message }); }
}
const result = {
  files: files.length, textures: textureCount, textureBytes,
  formats: [...formats].sort((a, b) => b[1] - a[1]).map(([format, count]) => ({ format, count, examples: examples.get(format) })),
  failureCount: failures.length,
  failures: failures.slice(0, 20)
};
console.log(JSON.stringify(result, null, 2));
