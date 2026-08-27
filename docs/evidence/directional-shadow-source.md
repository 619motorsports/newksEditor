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

The installed `ksShadowGenAT_vs.fxo` file has SHA-256 `8558a417dead16385fc9565c4882da6a8771f39b2ba4e1af47badb55284d1c85`.

It reads position from `v0` and texture coordinates from `v2`. It declares camera constants at `b0` and object constants at `b1`.

The installed `ksShadowGenAT_ps.fxo` file has SHA-256 `6782f08729c2dcd68254553f08ce6ad682e28f00fc9f493c48cd8c027ffc21f2`.

It samples the diffuse texture at `t0` with sampler `s3`. It reads the 32-byte material buffer at `b4`.

The alpha reference is the last float in that material buffer. The shader discards a fragment only when sampled alpha is below the reference.

These facts come from the installed shader containers, the matching PDB, and Ghidra analysis of the installed editor.

## Portable translation

The portable scene already skins retained vertices on the CPU. It preserves the complete 19-float stream for each vertex.

The translated shader reads only position at location zero. The pipeline still declares the complete stream layout.

The shader applies the 128-byte `DrawMatrices` contract. Vulkan uses push constants, and D3D12 uses root constants at `b0`.

The opaque and CPU-skinned pipelines have one vertex shader and no fragment shader. They have no shader resources.

The alpha-tested static pipeline has one vertex shader and one fragment shader. It uses the static 44-byte vertex stream.

The alpha fragment shader uses texture binding zero, sampler binding three, and material binding four. It preserves the recovered strict-less-than test.

The translated alpha vertex shader uses `DrawMatrices`. This replaces the native split between camera and object constant buffers.

The pipeline writes one single-sample D32 target. It uses `LESS`, depth test, depth write, solid fill, and back-face culling.

The static scene changes culling to none for a `double_face_shadow` packet. This behavior matches the recovered caster selection rule.

Explicit caller programs remain authoritative. The viewport adds translated programs only when the caller selects the built-in source.

The selector fills the opaque-static, alpha-tested static, and CPU-skinned roles.

## Maintained source and artifact identity

The Vulkan source is `native/src/render/shaders/directional_shadow_source.vert`.

Its SHA-256 is `0e97d9331b5f3f84b07018c48919c88a3f53e7efede1e07830c649678f3f65c1`.

The Vulkan SPIR-V artifact SHA-256 is `89175a3e81e7d1d90ceabd7276d7d6e1c9a8e5500371ed70351216c2a8a36848`.

The D3D12 source is `native/src/render/shaders/d3d12_directional_shadow_source.hlsl`.

Its SHA-256 is `c6b3ea63a4683f3f8b4d55cfacd5a397e89abb1a07113454621807e339bbc42a`.

The D3D12 DXBC artifact SHA-256 is `230657fe40f61e24ed83a125c88942b92590a2e1799aff47830d00f0992d6ccf`.

The alpha Vulkan vertex source is `native/src/render/shaders/directional_shadow_alpha_source.vert`.

Its SHA-256 is `f95028a212bead84c2337b7092ee13c456507c69ec24998a6a3bc38b91e6fc87`.

The alpha Vulkan fragment source is `native/src/render/shaders/directional_shadow_alpha_source.frag`.

Its SHA-256 is `ccadbe5e6417379209a9ff33e4e7bfed2e229f4eb087bc18b69d9c39842d0f67`.

The alpha SPIR-V vertex artifact SHA-256 is `cc091c2da560fb69329ac2d8ee52e7caf7101fa1f4697d948b6dcf5f1f7a9e34`.

The alpha SPIR-V fragment artifact SHA-256 is `f7bf802436687f734b2a61b2ebd0af65a229c94ccc5b38e4dcf8daa21efbf5c6`.

The alpha D3D12 source is `native/src/render/shaders/d3d12_directional_shadow_alpha_source.hlsl`.

Its SHA-256 is `ab020acba277fde726a3bacd99731a751788eb2d2409e6ba1b770624895a0842`.

The alpha DXBC vertex artifact SHA-256 is `8cbfd56edde0203e04da9b392edc5fe99739a97fc437ad7eb1f6683f04e4fc7a`.

The alpha DXBC pixel artifact SHA-256 is `5d78aaed5427c7d5d8c8d3ace4411ae10c945504cd809cdb808f02fcee308bd8`.

The generators store artifacts in `directional_shadow_source_artifacts.hpp` and `directional_shadow_alpha_source_artifacts.hpp`.

All programs use the `translated` provenance label.

Contract tests compare the source and artifact hashes. They also reject truncated, changed, relabeled, and wrong-layout programs.

## Fidelity boundary

This work does not implement the installed `cbBones b13` ABI. It does not claim native GPU-skinning parity.

The alpha sampler binding matches the installed `s3` register. The portable scene supplies the sampler state, so native sampler-state parity is not claimed.

The alpha program is a source translation. It does not claim byte identity with the installed Kunos shader containers.

The backends use cascade matrices that already contain their clip-space conversion. The translated shader does not add another conversion.
