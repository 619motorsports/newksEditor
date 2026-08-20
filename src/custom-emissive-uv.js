function finite(value) {
  const number = Number(value);
  return Number.isFinite(number) ? number : 0;
}

export function normalizeCustomEmissiveMirrorDirection(direction) {
  const x = finite(direction?.[0]);
  const y = finite(direction?.[1]);
  const length = Math.hypot(x, y);
  return length > 0 ? [x / length, y / length] : [0, 0];
}

function fract(value) {
  return value - Math.floor(value);
}

/** CPU reference for CSP's emMirrorUV branch in emissiveMapping.hlsl. */
export function customEmissiveUv(uv, mirrorUv = null, useRawUv = false) {
  const source = [finite(uv?.[0]), finite(uv?.[1])];
  const mapped = useRawUv ? source : source.map(fract);
  if (!mirrorUv) return mapped;

  const direction = normalizeCustomEmissiveMirrorDirection(mirrorUv.direction);
  const offset = finite(mirrorUv.offset);
  const signedDistance = direction[0] * mapped[0] + direction[1] * mapped[1] - offset;
  if (signedDistance >= 0 || (direction[0] === 0 && direction[1] === 0)) return mapped;

  return [
    mapped[0] - 2 * signedDistance * direction[0],
    mapped[1] - 2 * signedDistance * direction[1]
  ];
}
