export function customEmissiveAtlasSize(resolution, maximum = 1024, normalizedSize = 512) {
  const sourceWidth = Math.max(1, Number(resolution?.[0]) || 1);
  const sourceHeight = Math.max(1, Number(resolution?.[1]) || sourceWidth);
  const normalized = Math.max(sourceWidth, sourceHeight) <= 4;
  const scale = normalized
    ? normalizedSize / Math.max(sourceWidth, sourceHeight)
    : Math.min(1, maximum / sourceWidth, maximum / sourceHeight);
  return {
    sourceWidth,
    sourceHeight,
    width: Math.max(1, Math.round(sourceWidth * scale)),
    height: Math.max(1, Math.round(sourceHeight * scale)),
    normalized
  };
}

function finiteVector3(value) {
  return [0, 1, 2].map((index) => Number.isFinite(Number(value?.[index])) ? Number(value[index]) : 0);
}

export function customEmissiveVertexPosition(position, mirrorDirection = [1, 0, 0], mirrorOffset = 0) {
  const source = finiteVector3(position), direction = finiteVector3(mirrorDirection), offset = Number.isFinite(Number(mirrorOffset)) ? Number(mirrorOffset) : 0;
  const side = source[0] * direction[0] + source[1] * direction[1] + source[2] * direction[2] - offset;
  if (side >= 0) return { position: source, mirrored: false };
  const plane = direction.map((component) => component * offset), delta = source.map((component, index) => component - plane[index]);
  const projection = delta[0] * direction[0] + delta[1] * direction[1] + delta[2] * direction[2];
  return { position: delta.map((component, index) => plane[index] + component - 2 * projection * direction[index]), mirrored: true };
}

export function customEmissiveVertexMask(position, areas, mirrorDirection, mirrorOffset) {
  const mirrored = customEmissiveVertexPosition(position, mirrorDirection, mirrorOffset), sourceAreas = Array.from({ length: 4 }, (_, index) => areas?.[index] || { position: [0, 0, 0], weight: 0 });
  const distances = sourceAreas.map((area) => {
    const anchor = finiteVector3(area.position), weight = Number.isFinite(Number(area.weight)) ? Number(area.weight) : 0;
    const x = mirrored.position[0] - anchor[0], y = mirrored.position[1] - anchor[1], z = mirrored.position[2] - anchor[2];
    return (x * x + y * y + z * z) / Math.max(weight, 0.00001);
  });
  const minimum = Math.min(...distances);
  return { mask: distances.map((distance, index) => distance === minimum || Number(sourceAreas[index].weight) === 0 ? 1 : 0), mirrored: mirrored.mirrored, position: mirrored.position, distances };
}

export function applyCustomEmissiveVertexMask(channels, mask, mode = "multiply") {
  return Array.from({ length: 4 }, (_, index) => {
    const value = Math.max(0, Math.min(1, Number(channels?.[index]) || 0)), selected = Number(mask?.[index]) ? 1 : 0;
    if (mode === "add") return Math.min(1, value + selected);
    if (mode === "subtract") return Math.max(0, value - selected);
    return value * selected;
  });
}
