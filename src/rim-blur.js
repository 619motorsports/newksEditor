export const STOCK_RIM_CORNERS = Object.freeze(["LF", "RF", "LR", "RR"]);

function exactName(role, corner) {
  return role === "blurred" ? `RIM_BLUR_${corner}` : `RIM_${corner}`;
}

function sceneNodes(root) {
  if (!root || typeof root !== "object") return [];
  const output = [], stack = [root], visited = new Set();
  while (stack.length) {
    const node = stack.pop();
    if (!node || typeof node !== "object" || visited.has(node)) continue;
    visited.add(node); output.push(node);
    const children = Array.isArray(node.children) ? node.children : [];
    for (let index = children.length - 1; index >= 0; index--) stack.push(children[index]);
  }
  return output;
}

/** Identify an exact stock regular-rim or blurred-rim root name. */
export function stockRimNodeRole(value) {
  const name = String(value || "");
  for (const corner of STOCK_RIM_CORNERS) {
    if (name === exactName("regular", corner)) return { role: "regular", corner };
    if (name === exactName("blurred", corner)) return { role: "blurred", corner };
  }
  return null;
}

/** Audit the four regular and four blurred node names used by the native F1 path. */
export function auditStockRimNodes(root) {
  const nodes = sceneNodes(root), entries = STOCK_RIM_CORNERS.map((corner) => ({
    corner,
    regular: nodes.find((node) => node.name === exactName("regular", corner)) || null,
    blurred: nodes.find((node) => node.name === exactName("blurred", corner)) || null
  }));
  const regularNodes = entries.filter((entry) => entry.regular).map((entry) => ({ corner: entry.corner, node: entry.regular }));
  const blurredNodes = entries.filter((entry) => entry.blurred).map((entry) => ({ corner: entry.corner, node: entry.blurred }));
  const pairedCorners = entries.filter((entry) => entry.regular && entry.blurred).map((entry) => entry.corner);
  const regularStates = new Set(regularNodes.map((entry) => Boolean(entry.node.active)));
  const blurredStates = new Set(blurredNodes.map((entry) => Boolean(entry.node.active)));
  const missingRegular = entries.filter((entry) => !entry.regular).map((entry) => entry.corner);
  const missingBlurred = entries.filter((entry) => !entry.blurred).map((entry) => entry.corner);
  const warnings = [];
  if (!regularNodes.length) warnings.push("No regular RIM_* source node exists. The native F1 path needs at least one source node.");
  else if (missingRegular.length) warnings.push(`Missing regular rim nodes: ${missingRegular.map((corner) => exactName("regular", corner)).join(", ")}.`);
  if (missingBlurred.length) warnings.push(`Missing blurred rim nodes: ${missingBlurred.map((corner) => exactName("blurred", corner)).join(", ")}.`);
  if (regularStates.size > 1) warnings.push("Regular rim nodes have mixed authored active states. Native F1 uses the first regular node.");
  if (blurredStates.size > 1) warnings.push("Blurred rim nodes have mixed authored active states. The native status query uses the first blurred node.");
  return {
    entries, regularNodes, blurredNodes, pairedCorners, missingRegular, missingBlurred, warnings,
    available: regularNodes.length > 0,
    firstRegular: regularNodes[0] || null,
    firstBlurred: blurredNodes[0] || null,
    authoredRegularVisible: Boolean(regularNodes[0]?.node.active),
    authoredBlurredVisible: Boolean(blurredNodes[0]?.node.active)
  };
}

/** Return the blurred state produced by one native F1 edge. */
export function nativeRimBlurToggle(audit, currentPreview = null) {
  if (!audit?.firstRegular) return null;
  const regularActive = currentPreview === null ? Boolean(audit.firstRegular.node.active) : !Boolean(currentPreview);
  return regularActive;
}

/** Find the named rim root that owns a mesh path. */
export function stockRimRoleForPath(nodes) {
  for (const node of nodes || []) {
    const match = stockRimNodeRole(node?.name);
    if (match) return match;
  }
  return null;
}

/** Apply a canonical native rim state without changing the parsed KN5 nodes. */
export function rimPreviewBranchActive(nodes, blurredVisible) {
  for (const node of nodes || []) {
    const match = stockRimNodeRole(node?.name);
    const active = match ? (match.role === "blurred" ? Boolean(blurredVisible) : !Boolean(blurredVisible)) : Boolean(node?.active);
    if (!active) return false;
  }
  return true;
}
