# Native F4 damage evidence

This evidence was extracted from the installed Assetto Corsa SDK editor files. The
files are not redistributed.

- `ksNet.dll` SHA-256: `b38dcb826a3311d7233cf0a6a58e5da16b6c8679f8490091e7b434bf730091ca`
- `ksPerPixelMultiMap_damage_dirt.shader` SHA-256: `76d6a625c34e386641667a44a0433c6d1dc2af5be9e4a8ebbd6a886181f97dc8`
- Pixel DXBC SHA-256: `5470987d09ee724e6f8e7dffe7cbb06e7f44edce3d0dc7685e5f5266bbd8c44a`

The native C++/CLI IL was decoded with `dnfile` and `dncil`. The pixel DXBC was
extracted from the stock shader container and disassembled with the repository's
`tools/dxbc-disasm.c` utility through Wine's `D3DDisassemble`.

## F4 edge and five prefix calls

`ksNet.ksGraphics.render`, token `0x06000389`, RVA `0x27388`:

```text
IL_03c4: ldc.i4.s       115
IL_03c6: call           GetAsyncKeyState
IL_03d3: ldsflda        damageGlassNodesTrigger
IL_03d9: call           Trigger.ignoreSubsequentTrue
IL_03e3: ldsfld         areBrokenGlassShowed
IL_03e8: ldc.i4.0
IL_03e9: ceq
...
IL_041c: call           ksNet.showNodesWithPrefix
IL_045c: call           ksNet.showNodesWithPrefix
IL_049c: call           ksNet.showNodesWithPrefix
IL_04dc: call           ksNet.showNodesWithPrefix
IL_051c: call           ksNet.showNodesWithPrefix
IL_0521: ldsfld         areBrokenGlassShowed
IL_0526: ldc.i4.0
IL_0527: ceq
IL_052a: stsfld         areBrokenGlassShowed
```

The five UTF-16 operands at the calls decode, in order, to
`DAMAGE_GLASS_FRONT_`, `DAMAGE_GLASS_REAR_`, `DAMAGE_GLASS_LEFT_`,
`DAMAGE_GLASS_RIGHT_`, and `DAMAGE_GLASS_CENTER_`.

## Numbered lookup and shared material write

`ksNet.showNodesWithPrefix`, token `0x0600006f`, RVA `0x26ed4`:

```text
IL_000f: ldc.i4.1
IL_0021: ldarg.0
IL_0022: call           std.operator<<
IL_0027: ldc.i4.1
IL_0028: call           basic_ostream.<<
...
IL_0062: ldloc.0
IL_0063: brtrue.s       IL_006a
IL_0065: leave          IL_020a
IL_006a: ldloc.0
IL_006b: ldc.i4         184
IL_0071: ldarg.2
IL_0072: stind.i1
IL_0083: ldloc.0
IL_0086: call           ksNet.collectAllMeshes
...
IL_00ca: ldloc.3
IL_00cb: ldind.i4
IL_00cc: ldc.i4         248
IL_00d2: ldind.i4
IL_00d3: ldloca.s       "glassDamage"
IL_00d5: call           Material.getVar
IL_00da: stloc.2
IL_00f4: ldloc.2
IL_00f5: brfalse.s      IL_0109
IL_00f7: ldloc.2
IL_00f8: ldc.i4.s       24
IL_00fb: ldc.r4         1.0
IL_0100: stind.r4
IL_0101: ldloc.2
IL_0102: call           MaterialVar.set
...
IL_0190: ldloc.s        suffix
IL_0192: ldc.i4.1
IL_0193: add
```

The routine starts the decimal suffix at one, exits when lookup returns null,
writes the selected node's active byte, collects descendant meshes, and obtains
`glassDamage` through each mesh's material pointer. The write therefore changes a
shared material object, not only the descendant draw item. A missing material
variable skips the write.

## Stock pixel damage path

Relevant `ps_4_0` instructions:

```text
sample r4.xyzw, v3.xyxx, t21.xyzw, s0
sample r5.xyzw, v3.xyxx, t4.xyzw, s0
dp4 r1.w, r4.xyzw, cb5[2].xyzw
mul_sat r2.w, r5.w, r1.w
...
add r2.xyz, -r3.xyzx, r5.xyzx
mad r2.xyz, r2.w, r2.xyzx, r3.xyzx
...
sample r3.xyzw, v3.xyxx, t5.xyzw, s0
mul r1.w, r3.w, cb5[3].x
add r3.x, -r2.w, l(1.00000000e+00)
mad_sat r1.w, -r1.w, l(1.00000000e+01), l(1.00000000e+00)
add r3.y, r4.w, l(-1.00000000e+00)
mad r2.w, r2.w, r3.y, l(1.00000000e+00)
mul r1.w, r1.w, r3.x
mul r1.w, r2.w, r1.w
mul r2.w, r1.w, r6.x
```

Reflection maps t21 to `txDamageMask`, t4 to `txDamage`, t5 to the dirt texture,
`cb5[2]` to `damageZones`, and `cb5[3].x` to `dirt`. With `dirt == 0`, the dirt
factor becomes one and the final mapped-specular term reduces to the formula used
by the exact preview. A nonzero `dirt` enters additional stock operations, so Apex
does not label or enable its recovered damage shader path for that branch.
