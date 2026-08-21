export const COCKPIT_HIGH_ROOT = "COCKPIT_HR";
export const COCKPIT_LOW_ROOT = "COCKPIT_LR";

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

/** Identify one of the two exact cockpit roots used by the native F3 path. */
export function stockCockpitNodeRole(value) {
  if (value === COCKPIT_HIGH_ROOT) return "high";
  if (value === COCKPIT_LOW_ROOT) return "low";
  return null;
}

/** Audit the exact high- and low-resolution cockpit roots used by ksEditor. */
export function auditStockCockpitNodes(root) {
  const nodes = sceneNodes(root);
  const highNodes = nodes.filter((node) => node.name === COCKPIT_HIGH_ROOT);
  const lowNodes = nodes.filter((node) => node.name === COCKPIT_LOW_ROOT);
  const high = highNodes[0] || null, low = lowNodes[0] || null;
  const warnings = [];
  if (!high) warnings.push(`${COCKPIT_HIGH_ROOT} is missing. The native F3 path needs both cockpit roots.`);
  if (!low) warnings.push(`${COCKPIT_LOW_ROOT} is missing. The native F3 path needs both cockpit roots.`);
  if (highNodes.length > 1) warnings.push(`Found ${highNodes.length} exact ${COCKPIT_HIGH_ROOT} roots. Native F3 uses the first match.`);
  if (lowNodes.length > 1) warnings.push(`Found ${lowNodes.length} exact ${COCKPIT_LOW_ROOT} roots. Native F3 uses the first match.`);
  if (high && low && Boolean(high.active) === Boolean(low.active)) warnings.push("The cockpit roots have the same authored active state. Native F3 makes them mutually exclusive.");
  return {
    high, low, highNodes, lowNodes, warnings,
    available: Boolean(high && low),
    authoredHighVisible: Boolean(high?.active),
    authoredLowVisible: Boolean(low?.active)
  };
}

/** Return the high-resolution state produced by one native F3 edge. */
export function nativeCockpitToggle(audit, currentPreview = null) {
  if (!audit?.available) return null;
  return currentPreview === null ? !Boolean(audit.high.active) : !Boolean(currentPreview);
}

/** Find the exact cockpit root that owns a mesh path. */
export function stockCockpitRoleForPath(nodes) {
  for (const node of nodes || []) {
    const role = stockCockpitNodeRole(node?.name);
    if (role) return role;
  }
  return null;
}

/** Apply a native cockpit state to the audited pair without changing parsed KN5 nodes. */
export function cockpitPreviewBranchActive(nodes, highVisible, audit) {
  for (const node of nodes || []) {
    const role = node === audit?.high ? "high" : node === audit?.low ? "low" : null;
    const active = role ? (role === "high" ? Boolean(highVisible) : !Boolean(highVisible)) : Boolean(node?.active);
    if (!active) return false;
  }
  return true;
}
