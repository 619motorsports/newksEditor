# Headless Ghidra helpers

These scripts inspect the locally installed CSP binary without loading or shipping
it with Apex Editor.

- `ExportStringXrefs.java` scans raw ASCII/UTF-16 bytes and defined strings, then
  prints `APEX_XREF` rows for each matching native reference.
- `ExportDecompile.java` decompiles selected function entry addresses.
- `ExportDecompileSnippets.java` prints only the lines around requested address
  tokens in one decompiled function.
- `ExportDecompileLines.java` prints a selected one-based line range from a large
  decompiled function.
- `ExportFunctions.java` locates PDB or analysis-derived function names before
  targeted decompilation.
- `ExportFunctionXrefs.java` lists callers and other native references to selected
  function entry points.
- `ExportAddressXrefs.java` lists native references to arbitrary data or code
  addresses when no function or defined string anchors the target.
- `DumpScalars.java` prints little-endian integer and floating-point interpretations
  at selected addresses for validating optimized constant loads.

Example after importing `dwrite.dll` into a headless project:

```sh
/opt/ghidra/support/analyzeHeadless /tmp/apex-ghidra ApexDwrite \
  -process dwrite.dll -noanalysis \
  -scriptPath tools/ghidra \
  -postScript ExportStringXrefs.java LIGHT_SERIES CLUSTER_THRESHOLD
```

Set Ghidra's configuration directory to a writable location when the normal user
configuration directory is read-only. Treat decompiler output from optimized or
obfuscated regions as supporting evidence only.
