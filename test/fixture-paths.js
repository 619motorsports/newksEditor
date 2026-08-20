import { join } from "node:path";
import { fileURLToPath } from "node:url";

export const contentFixtureRoot = fileURLToPath(new URL("./content", import.meta.url));
export const carFixtureRoot = join(contentFixtureRoot, "cars", "619_gen6_arca_base");
export const trackFixtureRoot = join(contentFixtureRoot, "tracks", "sepang");

export const carMainKn5 = join(carFixtureRoot, "619_gen6_fusion13.kn5");
export const carColliderKn5 = join(carFixtureRoot, "collider.kn5");
export const trackMainKn5 = join(trackFixtureRoot, "sepang.kn5");

export function assettoPath(relativePath) {
  const root = process.env.ASSETTO_CORSA_ROOT;
  return root ? join(root, ...relativePath.split("/")) : undefined;
}
