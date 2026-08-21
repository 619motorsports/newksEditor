# Native authoring-grid evidence

The inspected binaries are the installed Assetto Corsa SDK files:

- `sdk/editor/ksEditor.exe` (512,000 bytes, SHA-256
  `7df6a75e7b8be9c6aae7f0ac09a66ac904a06f2a7e22fdbef635aec96c5144a0`)
- `sdk/editor/ksNet.dll` (17,211,904 bytes, SHA-256
  `b38dcb826a3311d7233cf0a6a58e5da16b6c8679f8490091e7b434bf730091ca`)

The excerpts below use IL offsets and metadata tokens from those exact binaries.

## UI toggle

`ksEditor.Form1.btnShowGrid_Click` has method token `0x060000e0` and RVA
`0x766c`. It reads the current value, compares it with false, and writes the
result:

```text
IL_0017: callvirt  MemberRef ksGraphics.getGridVisibility [0x0a0001c7]
IL_001c: ldc.i4.0
IL_001d: ceq
IL_001f: callvirt  MemberRef ksGraphics.setGridVisibility [0x0a0001c8]
```

The corresponding `ksNet.ksGraphics` accessors use the same static field:

```text
setGridVisibility, token 0x0600038d, RVA 0x21050
IL_000c: ldarg.1
IL_000d: stsfld    Field isGridVisible [0x04000234]

getGridVisibility, token 0x0600038e, RVA 0x21064
IL_000c: ldsfld    Field isGridVisible [0x04000234]
```

The `ksGraphics` static constructor, token `0x060003f5`, contains only `ret`.
The zero-initialized Boolean therefore makes the initial state hidden.

## Render state and geometry

`ksNet.ksGraphics.render` has method token `0x06000389` and RVA `0x27388`.
Before the conditional grid block it calls `GraphicsManager.setDepthMode` with
the integer value two. Enum metadata maps `eDepthNormal` to zero, `eNoZWrite`
to one, and `eOff` to two.

```text
IL_094d: ldfld     Field graphics [0x040003a3]
IL_0952: ldc.i4.2
IL_0953: call      MethodDef GraphicsManager.setDepthMode [0x06000252]
...
IL_0977: ldsfld    Field isGridVisible [0x04000234]
IL_097c: brfalse   2630
IL_0990: ldc.r4    1.0
IL_0995: ldc.r4    0.0
IL_099a: ldc.r4    1.0
IL_099f: call      MethodDef GLRenderer.color3f [0x06000251]
```

The first loop starts at −5, increments by one while the value is at most 5,
and emits `(−value, 0, −5)` to `(−value, 0, 5)`:

```text
IL_09a4: ldc.r4    -5.0
IL_09b3: ldloc.s   local(0x0007)
IL_09b5: neg
IL_09bb: ldc.r4    0.0
IL_09c0: ldc.r4    -5.0
IL_09c5: call      MethodDef GLRenderer.vertex3f [0x0600024f]
IL_09ca: ldloc.3
IL_09cb: ldloc.s   local(0x0017)
IL_09cd: ldc.r4    0.0
IL_09d2: ldc.r4    5.0
IL_09d7: call      MethodDef GLRenderer.vertex3f [0x0600024f]
IL_09e4: ldc.r4    1.0
IL_09e9: add
IL_09ee: ldc.r4    5.0
IL_09f3: ble.s     2475
```

The second loop has the same bounds and increment, and emits
`(−5, 0, −value)` to `(5, 0, −value)`:

```text
IL_09f5: ldc.r4    -5.0
IL_0a04: ldloc.s   local(0x0006)
IL_0a06: neg
IL_0a0a: ldc.r4    -5.0
IL_0a0f: ldc.r4    0.0
IL_0a14: ldloc.s   local(0x0016)
IL_0a16: call      MethodDef GLRenderer.vertex3f [0x0600024f]
IL_0a1c: ldc.r4    5.0
IL_0a21: ldc.r4    0.0
IL_0a26: ldloc.s   local(0x0016)
IL_0a28: call      MethodDef GLRenderer.vertex3f [0x0600024f]
IL_0a35: ldc.r4    1.0
IL_0a3a: add
IL_0a3f: ldc.r4    5.0
IL_0a44: ble.s     2556
```

This establishes the exact 22-segment, 10 m square, magenta XZ-plane grid and
its depth-disabled rendering state.
