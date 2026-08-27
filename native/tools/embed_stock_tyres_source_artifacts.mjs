#!/usr/bin/env node

import { readFileSync, writeFileSync } from 'node:fs';

const [output, ...inputs] = process.argv.slice(2);
if (!output || inputs.length !== 6) {
  throw new Error('usage: embed_stock_tyres_source_artifacts.mjs <output> <vert.spv> <frag.spv> <shadow.spv> <vert.dxbc> <frag.dxbc> <shadow.dxbc>');
}

const names = [
  'stock_tyres_vertex_spirv',
  'stock_tyres_fragment_spirv',
  'stock_tyres_shadow_fragment_spirv',
  'stock_tyres_vertex_dxbc',
  'stock_tyres_fragment_dxbc',
  'stock_tyres_shadow_fragment_dxbc',
];
const arrays = inputs.map((path, index) => {
  const bytes = readFileSync(path);
  const rows = [];
  for (let offset = 0; offset < bytes.length; offset += 12) {
    rows.push(`    ${[...bytes.subarray(offset, offset + 12)]
      .map(value => `0x${value.toString(16).padStart(2, '0')}U`)
      .join(', ')}`);
  }
  return `inline constexpr std::array<std::uint8_t, ${bytes.length}U> ${names[index]} = {\n${rows.join(',\n')}\n};`;
});

writeFileSync(output, `#pragma once\n\n#include <array>\n#include <cstdint>\n\nnamespace apex::render::generated {\n\n${arrays.join('\n\n')}\n\n}  // namespace apex::render::generated\n`);
