# ksNet blurred-rim IL evidence

This is a targeted IL capture from the Assetto Corsa SDK binary
`sdk/editor/ksNet.dll` (17,211,904 bytes, SHA-256
`b38dcb826a3311d7233cf0a6a58e5da16b6c8679f8490091e7b434bf730091ca`).
The file is not redistributed here.

The capture was produced from the CLR `MethodDef` table with `dnfile 0.18.0` and
`dncil 1.0.2`. Offsets below are IL offsets within each method. Opcode bytes are
included after each instruction.

## `ksGraphics.render`

Method token `0x06000389`, RVA `0x27388`.

The method passes virtual key 112 (F1) to `GetAsyncKeyState`, converts the result
to a Boolean, and passes it to `Trigger.ignoreSubsequentTrue`:

```text
000c  ldc.i4.s 112                         1f 70
000e  call 0x06000282 GetAsyncKeyState     28 82 02 00 06
0013  ldc.i4.0                             16
0014  bne.un.s 0019                        33 03
0016  ldc.i4.0                             16
0017  br.s 001a                            2b 01
0019  ldc.i4.1                             17
001a  stloc.s 0x5b                         13 5b
001c  ldsflda 0x0400023c                   7f 3c 02 00 04
0021  ldloc.s 0x5b                         11 5b
0023  call 0x0600025a Trigger.ignoreSubsequentTrue
                                              28 5a 02 00 06
0028  brfalse 02e3                        39 b6 02 00 00
```

The native wide-string fields used by the following lookup loops have these
`FieldRVA` values and contents:

```text
0x04000198  RVA 0x10fc9c  L"LF"
0x04000199  RVA 0x10fca4  L"RF"
0x0400019a  RVA 0x10fcac  L"LR"
0x0400019b  RVA 0x10fcb4  L"RR"
0x0400019c  RVA 0x10fcbc  L"RIM_BLUR_"
0x0400019e  RVA 0x10fcd0  L"RIM_"
```

The regular-rim lookup result vector is read at `01dc`. The first node's active
byte is read at offset 184. The vector loop writes the inverse value to offset 184
of each regular node:

```text
01dc  ldloca 0x85                          fe 0d 85 00
01e0  ldind.i4                             4a
01e1  ldind.i4                             4a
01e2  ldc.i4 184                           20 b8 00 00 00
01e7  add                                  58
01e8  ldind.u1                             47
01e9  stloc.s 0x23                         13 23
...
0214  ldloca 0x84                          fe 0d 84 00
0218  ldind.i4                             4a
0219  ldind.i4                             4a
021a  ldloc.s 0x23                         11 23
021c  ldc.i4.0                             16
021d  ceq                                  fe 01
021f  stloc.s 0x55                         13 55
0221  ldc.i4 184                           20 b8 00 00 00
0226  add                                  58
0227  ldloc.s 0x55                         11 55
0229  stind.i1                             52
```

The blurred-rim result-vector loop writes the original regular-rim state to the
same active-byte offset on every blurred node:

```text
0263  ldloca 0x82                          fe 0d 82 00
0267  ldind.i4                             4a
0268  ldind.i4                             4a
0269  ldc.i4 184                           20 b8 00 00 00
026e  add                                  58
026f  ldloc.s 0x23                         11 23
0271  stind.i1                             52
```

## `ksGraphics.areBlurredRimsVisible`

Method token `0x0600038b`, RVA `0x271b8`.

After the same `RIM_BLUR_` plus LF/RF/LR/RR lookup, the method tests the first
result. It reads its active byte at offset 184 when present, otherwise selects
false, and returns that Boolean:

```text
00da  ldloc.3                               09
00db  brtrue.s 00ec                         2d 0f
00dd  ldloc.1                               07
00de  ldc.i4.1                              17
00df  add                                   58
00e0  stloc.1                               0b
00e1  ldloc.0                               06
00e2  ldc.i4.s 24                           1f 18
00e4  add                                   58
00e5  stloc.0                               0a
00e6  ldloc.1                               07
00e7  ldc.i4.4                              1a
00e8  blt.un.s 0095                         37 ab
00ea  br.s 00f7                             2b 0b
00ec  ldloc.3                               09
00ed  ldc.i4 184                            20 b8 00 00 00
00f2  add                                   58
00f3  ldind.u1                              47
00f4  stloc.2                               0c
00f5  leave.s 0109                          de 12
00f7  ldc.i4.0                              16
00f8  stloc.2                               0c
...
0139  ldloc.2                               08
013a  ret                                   2a
```

These excerpts are the direct evidence for the exact key edge, node-name order,
active-byte offset, inversion, propagation, and status-query behavior implemented
by the portable preview.
