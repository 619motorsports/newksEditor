const finitePoint = (value) => Array.isArray(value) && value.length >= 3 && value.slice(0, 3).every((component) => Number.isFinite(Number(component)));

export function cspPointInOccluderPolygon(position, points) {
  if (!finitePoint(position) || !Array.isArray(points) || points.length < 3 || points.some((point) => !finitePoint(point))) return false;
  const x = Number(position[0]), z = Number(position[2]);
  let inside = false;
  for (let index = 0, previous = points.length - 1; index < points.length; previous = index++) {
    const a = points[index], b = points[previous], crosses = (Number(a[2]) > z) !== (Number(b[2]) > z);
    if (crosses && x < (Number(b[0]) - Number(a[0])) * (z - Number(a[2])) / (Number(b[2]) - Number(a[2])) + Number(a[0])) inside = !inside;
  }
  return inside;
}

function segmentIntersection2d(a, b, c, d) {
  const rx = b[0] - a[0], rz = b[2] - a[2], sx = d[0] - c[0], sz = d[2] - c[2], denominator = rx * sz - rz * sx;
  if (Math.abs(denominator) < 1e-9) return null;
  const qx = c[0] - a[0], qz = c[2] - a[2];
  return { ray: (qx * sz - qz * sx) / denominator, edge: (qx * rz - qz * rx) / denominator };
}

export function cspTrackOccluderBlocks(occluder, camera, target) {
  if (!occluder?.culling || !finitePoint(camera) || !finitePoint(target)) return false;
  if (occluder.exclusion?.length === 4 && cspPointInOccluderPolygon(camera, occluder.exclusion)) return false;
  const points = occluder.points || [], edgeCount = occluder.type === "box" ? points.length : Math.min(points.length - 1, 1);
  for (let index = 0; index < edgeCount; index++) {
    const a = points[index], b = points[(index + 1) % points.length], hit = segmentIntersection2d(camera, target, a, b);
    if (!hit || hit.ray <= 1e-6 || hit.ray >= 1 - 1e-6 || hit.edge < -1e-6 || hit.edge > 1 + 1e-6) continue;
    const rayHeight = camera[1] + (target[1] - camera[1]) * hit.ray, topHeight = a[1] + (b[1] - a[1]) * Math.max(0, Math.min(1, hit.edge));
    if (rayHeight <= topHeight) return true;
  }
  return false;
}

export function cspTrackOccluded(occluders, camera, target) {
  return Array.isArray(occluders) && occluders.some((occluder) => cspTrackOccluderBlocks(occluder, camera, target));
}
