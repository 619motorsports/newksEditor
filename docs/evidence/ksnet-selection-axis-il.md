# Native selected-node axis evidence

The inspected file is the installed `sdk/editor/ksNet.dll` (17,211,904 bytes,
SHA-256 `b38dcb826a3311d7233cf0a6a58e5da16b6c8679f8490091e7b434bf730091ca`).
The excerpts below are from `ksNet.ksGraphics.render`, method token
`0x06000389`, RVA `0x27388`.

## Selection and world matrix

The marker follows the authoring-grid pass. A null selected-node argument skips
the block. Otherwise the code calls the selected native node's vtable entry at
byte offset 32 and copies the returned 64-byte matrix. Matrix byte offsets 48,
52, and 56 become the common line origin:

```text
IL_0a46: ldarg.2
IL_0a47: brfalse   3138
IL_0a4c: ldarg.2
IL_0a4d: ldfld     Field nNode [0x0400038c]
IL_0a5a: ldind.i4
IL_0a5b: ldc.i4.s  32
IL_0a5d: add
IL_0a5e: ldind.i4
IL_0a5f: calli     StandAloneSig 193 [0x110000c1]
IL_0a6a: ldc.i4.s  64
IL_0a6c: cpblk
IL_0a72: ldc.i4.s  48
IL_0a75: ldind.r4
IL_0a7d: ldc.i4.s  52
IL_0a80: ldind.r4
IL_0a88: ldc.i4.s  56
IL_0a8b: ldind.r4
```

## Red, green, and blue segments

The renderer begins a line list and emits red from the origin. It copies matrix
offsets 0, 4, and 8, normalizes that vector, adds it to the origin, and emits the
endpoint:

```text
IL_0a9c: ldc.i4.0
IL_0a9d: ldc.i4.0
IL_0a9e: call       MethodDef GLRenderer.begin [0x06000250]
IL_0aa4: ldc.r4     1.0
IL_0aa9: ldc.r4     0.0
IL_0aae: ldc.r4     0.0
IL_0ab3: call       MethodDef GLRenderer.color3f [0x06000251]
IL_0ab9: ldloca.s   local(0x0066)
IL_0abb: call       MethodDef GLRenderer.vertex3f [0x0600024d]
IL_0ac4: ldind.r4
IL_0acc: ldc.i4.4
IL_0ad6: ldc.i4.8
IL_0ae0: call       MethodDef vec3f.normalize [0x06000051]
IL_0ae8: ldloca.s   local(0x0066)
IL_0aeb: add
IL_0b1e: call       MethodDef GLRenderer.vertex3f [0x0600024d]
```

Green repeats the sequence with color `(0, 1, 0)` and matrix byte offsets 16,
20, and 24:

```text
IL_0b24: ldc.r4     0.0
IL_0b29: ldc.r4     1.0
IL_0b2e: ldc.r4     0.0
IL_0b33: call       MethodDef GLRenderer.color3f [0x06000251]
IL_0b44: ldc.i4.s   16
IL_0b4f: ldc.i4.s   20
IL_0b5a: ldc.i4.s   24
IL_0b65: call       MethodDef vec3f.normalize [0x06000051]
IL_0ba3: call       MethodDef GLRenderer.vertex3f [0x0600024d]
```

Blue uses `(0, 0, 1)`, reads matrix offsets 32, 36, and 40, negates all three
components, normalizes, and adds the result to the origin:

```text
IL_0ba9: ldc.r4     0.0
IL_0bae: ldc.r4     0.0
IL_0bb3: ldc.r4     1.0
IL_0bb8: call       MethodDef GLRenderer.color3f [0x06000251]
IL_0bc7: ldc.i4.s   32
IL_0bcb: neg
IL_0bd0: ldc.i4.s   36
IL_0bd4: neg
IL_0bd9: ldc.i4.s   40
IL_0bdd: neg
IL_0bf9: call       MethodDef vec3f.normalize [0x06000051]
IL_0c37: call       MethodDef GLRenderer.vertex3f [0x0600024d]
IL_0c3d: call       MethodDef GLRenderer.end [0x0600024e]
```

Because each normalized basis is added directly to the origin, every segment is
one world meter long. The depth mode remains `eOff` (integer two) from the grid
and marker setup, then the method restores `eDepthNormal` (integer zero):

```text
IL_094d: ldfld     Field graphics [0x040003a3]
IL_0952: ldc.i4.2
IL_0953: call      MethodDef GraphicsManager.setDepthMode [0x06000252]
...
IL_0c42: ldarg.0
IL_0c43: ldfld     Field graphics [0x040003a3]
IL_0c48: ldc.i4.0
IL_0c49: call      MethodDef GraphicsManager.setDepthMode [0x06000252]
```
