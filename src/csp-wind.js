export const CSP_WIND_MAP_SIZE = 64;
export const CSP_WIND_PARTICLE_COUNT = 32;
export const CSP_WIND_MAP_FORMAT = "r16f-linear-repeat";

const saturate = (value) => Math.max(0, Math.min(1, value));
const fract = (value) => value - Math.floor(value);

export function createCspWindParticles() {
  return new Float32Array(CSP_WIND_PARTICLE_COUNT * 4);
}

export function updateCspWindParticles(particles, deltaSeconds, velocity, random = Math.random) {
  const delta = Math.max(0, Number(deltaSeconds) || 0), dx = (Number(velocity?.[0]) || 0) * delta, dz = (Number(velocity?.[1]) || 0) * delta;
  for (let offset = 0; offset < particles.length; offset += 4) {
    const life = particles[offset] - (particles[offset + 3] + 0.5) * delta * 0.5;
    if (life >= 0) {
      particles[offset] = life;
      particles[offset + 1] += dx;
      particles[offset + 2] += dz;
    } else {
      const lifetimeRandom = random(), z = random() * 2000, x = random() * 2000;
      particles[offset] = 1;
      particles[offset + 1] = x;
      particles[offset + 2] = z;
      particles[offset + 3] = lifetimeRandom;
    }
  }
  return [dx, dz];
}

export function cspWindDecay(speedMetersPerSecond) {
  return 0.99 - saturate((Number(speedMetersPerSecond) || 0) / 35) * 0.02;
}

export function cspWindMapValue(previousValue, uv, particles, windDelta, speedMetersPerSecond) {
  let added = 0;
  for (let offset = 0; offset < particles.length; offset += 4) {
    const life = particles[offset], randomA = fract(particles[offset + 3] * 496.411011), randomB = fract(particles[offset + 3] * 899.885986);
    let dx = fract(particles[offset + 1] * 0.05) - uv[0], dz = fract(particles[offset + 2] * 0.05) - uv[1];
    if (dx > 0.5) dx -= 1; else if (dx < -0.5) dx += 1;
    if (dz > 0.5) dz -= 1; else if (dz < -0.5) dz += 1;
    let distanceSquared = (dx * dx + dz * dz) * 0.7;
    for (let step = -3; step <= 3; step++) {
      const lineX = dx + step * windDelta[0] / 12, lineZ = dz + step * windDelta[1] / 12;
      distanceSquared = Math.min(distanceSquared, (lineX * lineX + lineZ * lineZ) * (0.7 + Math.abs(step) * 0.07));
    }
    const lifeTriangle = Math.max(0, 0.5 - Math.abs(0.5 - life));
    if (lifeTriangle <= 0) continue;
    const radial = Math.max(0, 1 - 2 * Math.sqrt(distanceSquared) * (3 + 2 * randomA) / Math.sqrt(lifeTriangle));
    added += radial * lifeTriangle * lifeTriangle * (0.2 + 0.2 * randomB) * 0.3;
  }
  return Math.max(0, Math.min(4, (Number(previousValue) || 0) * cspWindDecay(speedMetersPerSecond) + added));
}
