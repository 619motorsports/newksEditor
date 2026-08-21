export const DAMAGE_GLASS_PREFIXES = Object.freeze([
  "DAMAGE_GLASS_FRONT_",
  "DAMAGE_GLASS_REAR_",
  "DAMAGE_GLASS_LEFT_",
  "DAMAGE_GLASS_RIGHT_",
  "DAMAGE_GLASS_CENTER_"
]);

export const STOCK_DAMAGE_DIRT_SHADER = "ksPerPixelMultiMap_damage_dirt";

function sceneNodes(root) {
  if (!root || typeof root !== "object") return [];
  const output = [], stack = [root], visited = new Set();
  while (stack.length) {
    const node = stack.pop();
    if (!node || typeof node !== "object" || visited.has(node)) continue;
    visited.add(node);
    output.push(node);
    const children = Array.isArray(node.children) ? node.children : [];
    for (let index = children.length - 1; index >= 0; index--) stack.push(children[index]);
  }
  return output;
}

function property(material, name) {
  return material?.properties?.find((entry) => entry.name === name) || null;
}

function resource(material, slot) {
  return material?.resources?.find((entry) => entry.slot === slot) || null;
}

export function isStockDamageDirtShader(value) {
  return value === STOCK_DAMAGE_DIRT_SHADER;
}

/** The recovered stock response is exact only for the dirt-zero shader branch. */
export function isStockDamageDirtZero(shader, dirt = 0) {
  return isStockDamageDirtShader(shader) && Number(dirt) === 0;
}

/** Audit the five exact, numbered node sequences used by native F4. */
export function auditNativeDamagePreview(root, materials = []) {
  const nodes = sceneNodes(root), roots = [], warnings = [];
  const groups = DAMAGE_GLASS_PREFIXES.map((prefix) => {
    const matches = new Map();
    for (const node of nodes) {
      const suffix = node.name?.startsWith(prefix) ? node.name.slice(prefix.length) : "";
      if (!/^[1-9]\d*$/.test(suffix)) continue;
      const index = Number(suffix);
      if (!Number.isSafeInteger(index)) continue;
      const list = matches.get(index) || [];
      list.push(node);
      matches.set(index, list);
    }
    const selected = [];
    for (let index = 1; matches.has(index); index++) {
      const candidates = matches.get(index);
      selected.push(candidates[0]);
      roots.push(candidates[0]);
      if (candidates.length > 1) warnings.push(`Found ${candidates.length} exact ${prefix}${index} nodes. Native F4 uses the first match.`);
    }
    const firstMissing = selected.length + 1;
    const ignored = [...matches.entries()].filter(([index]) => index > firstMissing).flatMap(([, candidates]) => candidates);
    if (ignored.length) warnings.push(`${prefix}${firstMissing} is missing. Native F4 ignores ${ignored.length} later node${ignored.length === 1 ? "" : "s"}.`);
    return { prefix, selected, ignored, firstMissing };
  });
  const materialEntries = (materials || []).map((material, materialId) => {
    const damageZones = property(material, "damageZones");
    if (!damageZones) return null;
    const exactShader = isStockDamageDirtShader(material.shader);
    const dirt = Number(property(material, "dirt")?.value ?? 0);
    const missingResources = exactShader
      ? ["txDamage", "txDamageMask"].filter((slot) => !resource(material, slot))
      : [];
    if (missingResources.length) warnings.push(`${material.name || `Material ${materialId}`} is missing ${missingResources.join(", ")}.`);
    if (exactShader && dirt !== 0) warnings.push(`${material.name || `Material ${materialId}`} has dirt ${Number.isFinite(dirt) ? dirt : "that is not finite"}; the exact preview supports only the recovered dirt-zero branch.`);
    return { material, materialId, damageZones, missingResources, dirt, exactShader, exactZeroDirt: exactShader && dirt === 0 };
  }).filter(Boolean);
  const rootSet = new WeakSet(roots);
  return {
    groups, roots, rootSet, warnings, materialEntries,
    available: roots.length > 0 || materialEntries.length > 0,
    exactMaterials: materialEntries.filter((entry) => entry.exactZeroDirt && !entry.missingResources.length),
    authoredBrokenVisible: roots.some((node) => Boolean(node.active)),
    authoredDamageVisible: materialEntries.some((entry) => entry.damageZones.value4?.some((value) => Number(value) > 0))
  };
}

/** Return the broken state produced by one native F4 edge. */
export function nativeDamageToggle(audit, currentPreview = null) {
  if (!audit?.available) return null;
  return currentPreview === null ? true : !Boolean(currentPreview);
}

/** Find a native-selected damage-glass root in a mesh path. */
export function nativeDamageRootForPath(nodes, audit) {
  for (const node of nodes || []) if (audit?.rootSet?.has(node)) return node;
  return null;
}

/** Apply the native damage-glass active state without changing parsed nodes. */
export function damagePreviewBranchActive(nodes, audit, brokenVisible) {
  for (const node of nodes || []) {
    const active = audit?.rootSet?.has(node) ? Boolean(brokenVisible) : Boolean(node?.active);
    if (!active) return false;
  }
  return true;
}

/** Collect shared material objects written by native F4 through selected descendants. */
export function nativeDamageGlassMaterials(items, audit) {
  const affected = new Set();
  for (const item of items || []) {
    if (!nativeDamageRootForPath(item?.ancestors, audit)) continue;
    if (property(item?.material, "glassDamage")) affected.add(item.material);
  }
  return affected;
}

/** Apply the native one-way shared-material glassDamage write after any F4 edge. */
export function nativeDamageGlassValue(material, affectedMaterials, currentPreview, authoredValue) {
  return currentPreview !== null && affectedMaterials?.has(material) ? 1 : authoredValue;
}

/** Reproduce the damage factor at instructions 44 through 47 of the stock pixel shader. */
export function stockDamageAmount(damageAlpha, mask, zones) {
  const dot = [0, 1, 2, 3].reduce((sum, index) => sum + Number(mask?.[index] || 0) * Number(zones?.[index] || 0), 0);
  return Math.max(0, Math.min(1, Number(damageAlpha || 0) * dot));
}

/** Reproduce the stock diffuse and dirt-zero specular damage stage. */
export function stockDamageResponse(diffuse, damage, mapsRed, normalAlpha, amount) {
  const factor = Math.max(0, Math.min(1, Number(amount) || 0));
  return {
    diffuse: [0, 1, 2].map((index) => Number(diffuse?.[index] || 0) * (1 - factor) + Number(damage?.[index] || 0) * factor),
    specularMap: Number(mapsRed || 0) * (1 - factor) * (1 + factor * (Number(normalAlpha ?? 1) - 1))
  };
}
