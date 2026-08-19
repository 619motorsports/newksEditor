function filePath(file) {
  return String(file?.webkitRelativePath || file?.relativePath || file?.name || "");
}

export function normalizeAssetPath(value) {
  const source = String(value || "").trim().replace(/^(['"])(.*)\1$/, "$2").replace(/^\?+/, "");
  const parts = source.replaceAll("\\", "/").split("/");
  const normalized = [];
  for (const raw of parts) {
    const part = raw.trim();
    if (!part || part === ".") continue;
    if (part === "..") normalized.pop();
    else normalized.push(part);
  }
  return normalized.join("/");
}

function add(map, key, entry) {
  if (!key) return;
  const normalized = key.toLowerCase(), existing = map.get(normalized);
  if (existing) existing.push(entry);
  else map.set(normalized, [entry]);
}

export function createAssetFileIndex(files) {
  const entries = [], exact = new Map(), basenames = new Map();
  for (const file of files || []) {
    const path = normalizeAssetPath(filePath(file));
    if (!path) continue;
    const parts = path.split("/"), relativePath = parts.length > 1 ? parts.slice(1).join("/") : path;
    const entry = { file, path, relativePath };
    entries.push(entry);
    add(exact, path, entry);
    if (relativePath.toLowerCase() !== path.toLowerCase()) add(exact, relativePath, entry);
    add(basenames, parts.at(-1), entry);
  }
  return { entries, exact, basenames };
}

function resolution(status, requestedPath, matches = [], matchedBy = "") {
  return { status, requestedPath, matches, file: matches.length === 1 ? matches[0].file : null, path: matches.length === 1 ? matches[0].path : "", matchedBy };
}

export function resolveAssetFile(index, requested) {
  const requestedPath = normalizeAssetPath(requested), key = requestedPath.toLowerCase();
  if (!key) return resolution("missing", requestedPath);
  const exact = index?.exact?.get(key) || [];
  if (exact.length === 1) return resolution("resolved", requestedPath, exact, "exact");
  if (exact.length > 1) return resolution("ambiguous", requestedPath, exact, "exact");

  const suffix = (index?.entries || []).filter((entry) => entry.path.toLowerCase().endsWith(`/${key}`));
  if (suffix.length === 1) return resolution("resolved", requestedPath, suffix, "suffix");
  if (suffix.length > 1) return resolution("ambiguous", requestedPath, suffix, "suffix");

  const basename = key.split("/").at(-1), named = index?.basenames?.get(basename) || [];
  if (named.length === 1) return resolution("resolved", requestedPath, named, "basename");
  if (named.length > 1) return resolution("ambiguous", requestedPath, named, "basename");
  return resolution("missing", requestedPath);
}

export function externalResourcePaths(evaluation) {
  const paths = new Map();
  for (const override of evaluation?.nodeOverrides?.values() || []) {
    for (const resource of override.resources?.values() || []) {
      const path = normalizeAssetPath(resource?.file);
      if (path && !paths.has(path.toLowerCase())) paths.set(path.toLowerCase(), path);
    }
  }
  return [...paths.values()].sort((a, b) => a.localeCompare(b));
}

const SKIN_TEXTURE_EXTENSION = /\.(?:dds|png|jpe?g|webp)$/i;

export function discoverAssetSkins(index) {
  const groups = new Map();
  for (const entry of index?.entries || []) {
    const match = entry.relativePath.match(/^skins\/([^/]+)\/([^/]+)$/i);
    if (!match || !SKIN_TEXTURE_EXTENSION.test(match[2])) continue;
    const name = match[1], key = name.toLowerCase();
    if (!groups.has(key)) groups.set(key, { name, path: `skins/${name}`, files: [] });
    groups.get(key).files.push({ ...entry, basename: match[2] });
  }
  return [...groups.values()].map((skin) => ({ ...skin, files: skin.files.sort((a, b) => a.basename.localeCompare(b.basename)) })).sort((a, b) => a.name.localeCompare(b.name, undefined, { numeric: true }));
}

export function discoverAssetAnimations(index) {
  return (index?.entries || []).filter((entry) => /\.ksanim$/i.test(entry.relativePath)).sort((a, b) => a.relativePath.localeCompare(b.relativePath, undefined, { numeric: true }));
}

function basename(value) {
  return normalizeAssetPath(value).split("/").at(-1) || "";
}

export function matchSkinTextures(skinFiles, textureNames) {
  const byName = new Map();
  for (const entry of skinFiles || []) {
    const name = basename(entry.basename || entry.relativePath || entry.path || entry.file?.name).toLowerCase();
    if (!name) continue;
    const matches = byName.get(name) || [];
    matches.push(entry);
    byName.set(name, matches);
  }
  const files = [], ambiguous = [], missing = [];
  for (const rawName of [...new Set([...(textureNames || [])].map((name) => basename(name).toLowerCase()).filter(Boolean))]) {
    const matches = byName.get(rawName) || [];
    if (matches.length === 1) files.push({ name: rawName, entry: matches[0] });
    else if (matches.length > 1) ambiguous.push({ name: rawName, entries: matches });
    else missing.push(rawName);
  }
  return { files, ambiguous, missing };
}
