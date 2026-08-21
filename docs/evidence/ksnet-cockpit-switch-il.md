# ksNet cockpit-switch IL evidence

This is a targeted IL capture from the Assetto Corsa SDK binary
`sdk/editor/ksNet.dll` (17,211,904 bytes, SHA-256
`b38dcb826a3311d7233cf0a6a58e5da16b6c8679f8490091e7b434bf730091ca`).
The file is not redistributed here.

The capture was produced from the CLR `MethodDef` table with `dnfile 0.18.0` and
`dncil 1.0.2`. Offsets are IL offsets within `ksGraphics.render`, method token
`0x06000389`, RVA `0x27388`. Opcode bytes follow each instruction.

The method passes virtual key 114 (F3) to `GetAsyncKeyState`, converts the result
to a Boolean, and passes it to `Trigger.ignoreSubsequentTrue`:

```text
02e3  ldc.i4.s 114                         1f 72
02e5  call 0x06000282 GetAsyncKeyState     28 82 02 00 06
02ea  ldc.i4.0                             16
02eb  bne.un.s 02f0                        33 03
02ed  ldc.i4.0                             16
02ee  br.s 02f1                            2b 01
02f0  ldc.i4.1                             17
02f1  stloc.s 0x51                         13 51
02f3  ldsflda 0x0400023a                   7f 3a 02 00 04
02f8  ldloc.s 0x51                         11 51
02fa  call 0x0600025a Trigger.ignoreSubsequentTrue
                                              28 5a 02 00 06
02ff  brfalse 03c4                         39 c0 00 00 00
```

The exact native wide-string fields used by the two recursive lookups are:

```text
0x0400019d  RVA 0x10fb74  L"COCKPIT_HR"
0x0400019f  RVA 0x10fcdc  L"COCKPIT_LR"
```

The low lookup result is stored in local `0x22`; the high lookup result is stored
in local `0x13`. The branch requires both first-match pointers:

```text
0308  ldsflda 0x0400019f L"COCKPIT_LR"    7f 9f 01 00 04
...
0331  stloc.s 0x22                         13 22
...
0352  ldsflda 0x0400019d L"COCKPIT_HR"    7f 9d 01 00 04
...
0379  stloc.s 0x13                         13 13
...
0394  ldloc.s 0x22                         11 22
0396  brfalse.s 03c4                       2c 2c
0398  ldloc.s 0x13                         11 13
039a  brfalse.s 03c4                       2c 28
```

The method reads the high node's active byte at offset 184, inverts it, writes the
new state back to the high node, inverts it again, and writes the opposite state to
the low node:

```text
039c  ldloc.s 0x13                         11 13
039e  ldc.i4 184                           20 b8 00 00 00
03a3  add                                  58
03a4  ldind.u1                             47
03a5  stloc.1                              0b
03a6  ldloc.1                              07
03a7  ldc.i4.0                             16
03a8  ceq                                  fe 01
03aa  stloc.1                              0b
03ab  ldloc.s 0x13                         11 13
03ad  ldc.i4 184                           20 b8 00 00 00
03b2  add                                  58
03b3  ldloc.1                              07
03b4  stind.i1                             52
03b5  ldloc.1                              07
03b6  ldc.i4.0                             16
03b7  ceq                                  fe 01
03b9  stloc.1                              0b
03ba  ldloc.s 0x22                         11 22
03bc  ldc.i4 184                           20 b8 00 00 00
03c1  add                                  58
03c2  ldloc.1                              07
03c3  stind.i1                             52
```

Because each recursive lookup produces one pointer, duplicate exact names outside
this audited pair retain their authored active state in the portable preview.
