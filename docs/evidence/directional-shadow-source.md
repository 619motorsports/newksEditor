# Directional shadow source translation

## Scope

This note records the built-in directional shadow caster programs for Vulkan and D3D12.

The programs implement the existing portable retained-geometry contract. They do not translate installed Kunos shader bytecode.

The skinned role consumes a 76-byte vertex stream after CPU skinning. It does not use the native bone buffer.

## Installed editor evidence

The installed `ksShadowGenSKIN_vs.fxo` file has SHA-256 `e2e7a6f2a18814832b2cc13271d4872c713fbbbbadf266d9cb24a5ffe4437fab`.

Its RDEF data declares `cbCamera` at `b0`. It declares a 3,520-byte `cbBones` buffer at `b13`.

The bone buffer contains 55 matrices. The shader reads four weights and four indices from each vertex.

The shader converts each used index to an integer. It does not do a bounds test before the bone-buffer read.

The installed `ksShadowGenSKIN_ps.fxo` file has SHA-256 `78bc800223f4fc2a4c75616ee454a25e4b5c05a6e150d2514fb573b9d48873d4`.

This file is byte-identical to `ksShadowGen_ps.fxo`. Its shader writes zero and does not sample a material texture.

`MaterialFilterSM::apply` selects the skinned program before it tests the material blend mode.

`SkinnedMesh::updateBonesBuffer` writes `boneCount * 64` bytes to slot 13 before the draw.

These facts come from the installed shader containers, the matching PDB, and Ghidra analysis of the installed editor.

## Portable translation

The portable scene already skins retained vertices on the CPU. It preserves the complete 19-float stream for each vertex.

The translated shader reads only position at location zero. The pipeline still declares the complete stream layout.

The shader applies the 128-byte `DrawMatrices` contract. Vulkan uses push constants, and D3D12 uses root constants at `b0`.

The pipeline has one vertex shader and no fragment shader. It has no shader resources.

The pipeline writes one single-sample D32 target. It uses `LESS`, depth test, depth write, solid fill, and back-face culling.

The static scene changes culling to none for a `double_face_shadow` packet. This behavior matches the recovered caster selection rule.

Explicit caller programs remain authoritative. The viewport adds translated programs only when the caller selects the built-in source.

The selector fills the opaque-static and CPU-skinned roles. It does not fill the alpha-tested static role.

## Maintained source and artifact identity

The Vulkan source is `native/src/render/shaders/directional_shadow_source.vert`.

Its SHA-256 is `0e97d9331b5f3f84b07018c48919c88a3f53e7efede1e07830c649678f3f65c1`.

The Vulkan SPIR-V artifact SHA-256 is `89175a3e81e7d1d90ceabd7276d7d6e1c9a8e5500371ed70351216c2a8a36848`.

The D3D12 source is `native/src/render/shaders/d3d12_directional_shadow_source.hlsl`.

Its SHA-256 is `c6b3ea63a4683f3f8b4d55cfacd5a397e89abb1a07113454621807e339bbc42a`.

The D3D12 DXBC artifact SHA-256 is `230657fe40f61e24ed83a125c88942b92590a2e1799aff47830d00f0992d6ccf`.

The generator stores both artifacts in `directional_shadow_source_artifacts.hpp`. The programs use the `translated` provenance label.

Contract tests compare the source and artifact hashes. They also reject truncated, changed, relabeled, and wrong-layout programs.

## Fidelity boundary

This work does not implement the installed `cbBones b13` ABI. It does not claim native GPU-skinning parity.

This work does not add alpha-tested shader translation. The explicit `t0`, `s3`, and `b4` alpha contract remains separate.

The backends use cascade matrices that already contain their clip-space conversion. The translated shader does not add another conversion.
