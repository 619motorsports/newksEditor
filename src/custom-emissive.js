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
