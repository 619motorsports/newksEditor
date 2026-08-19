import test from "node:test";
import assert from "node:assert/strict";
import { createCspWindParticles, cspWindDecay, cspWindMapValue, CSP_WIND_MAP_FORMAT, CSP_WIND_MAP_SIZE, CSP_WIND_PARTICLE_COUNT, updateCspWindParticles } from "../src/csp-wind.js";

test("matches CSP wind particle respawn, lifetime, and advection constants", () => {
  const particles = createCspWindParticles(), values = Array.from({ length: CSP_WIND_PARTICLE_COUNT * 3 }, (_, index) => ((index % 10) + 1) / 10); let cursor = 0;
  const delta = updateCspWindParticles(particles, 0.1, [4, -2], () => values[cursor++]);
  assert.deepEqual(delta, [0.4, -0.2]);
  assert.deepEqual(Array.from(particles.slice(0, 3)), [1, 600, 400]);
  assert.ok(Math.abs(particles[3] - 0.1) < 1e-7);
  updateCspWindParticles(particles, 0.1, [4, -2], () => 0);
  assert.ok(Math.abs(particles[0] - 0.97) < 1e-6);
  assert.ok(Math.abs(particles[1] - 600.4) < 1e-4);
  assert.ok(Math.abs(particles[2] - 399.8) < 1e-4);
});

test("matches the disassembled accWind decay and centered pulse equation", () => {
  const particle = new Float32Array([0.5, 0, 0, 0]);
  assert.equal(cspWindDecay(0), 0.99);
  assert.equal(cspWindDecay(35), 0.97);
  assert.ok(Math.abs(cspWindMapValue(1, [0, 0], particle, [0, 0], 0) - 1.005) < 1e-9);
  assert.equal(cspWindMapValue(0, [0.5, 0.5], particle, [0, 0], 0), 0);
  assert.equal(CSP_WIND_MAP_SIZE, 64);
  assert.equal(CSP_WIND_PARTICLE_COUNT, 32);
  assert.equal(CSP_WIND_MAP_FORMAT, "r16f-linear-repeat");
});
