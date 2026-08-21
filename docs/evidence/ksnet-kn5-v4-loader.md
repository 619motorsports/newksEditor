# KN5 version 4 loader evidence

The inspected binary is the installed `sdk/editor/ksNet.dll` (17,211,904 bytes,
SHA-256 `b38dcb826a3311d7233cf0a6a58e5da16b6c8679f8490091e7b434bf730091ca`).
Function names and addresses come from the shipped PDB. The C-like excerpts are
Ghidra decompiler output and retain its generated local names.

The two installed v4 axis fixtures are byte-identical, 26,660 bytes, and have
SHA-256 `4eb803b4a7bb6a118b3db8e7c383b69bf7197452535c7b550d989223adca53e8`:

- `content/objects3D/axis.kn5`
- `sdk/editor/content/objects3D/axis.kn5`

## Header version gate

`KN5IO::load` at `0x1003ade4` reads the version into `loadingVersion`. It reads
the following four-byte source marker only when that value is greater than 5:

```c
std::basic_istream<char,std::char_traits<char>_>::read(
    (basic_istream<char,std::char_traits<char>_> *)auStack_cc,
    auStack_114,
    (ulonglong)uVar12 << 0x20);
this->loadingVersion = auStack_114._0_4_;
if (5 < this->loadingVersion) {
  std::basic_istream<char,std::char_traits<char>_>::read(
      (basic_istream<char,std::char_traits<char>_> *)auStack_cc,
      (char *)&uStack_118,
      (ulonglong)uVar12 << 0x20);
}
loadMaterialsBinary(this, ...);
if (this->loadingVersion == 1) loadBinaryV1(this, ...);
else loadBinaryV2(this, ...);
```

Thus v4 and v5 proceed directly from the version word to the texture count.

## Material depth gate and default

`Material::Material` at `0x1003fcc7` initializes both blend and depth state:

```c
this->depthMode = eDepthNormal;
this->blendMode = eOpaque;
this->cullMode = eCullFront;
...
this->depthMode = eDepthNormal;
```

`KN5IO::loadMaterialsBinary` at `0x1003b9de` then reads the two one-byte blend
flags. Its serialized depth-mode read is conditional on a version greater than
4:

```c
std::basic_istream<char,std::char_traits<char>_>::read(... alphaBlend ...);
std::basic_istream<char,std::char_traits<char>_>::read(... alphaToCoverage ...);
if (4 < this->loadingVersion) {
  std::basic_istream<char,std::char_traits<char>_>::read(
      (basic_istream<char,std::char_traits<char>_> *)this_00,
      (char *)&local_1a8,
      (ulonglong)unaff_EDI << 0x20);
  if (local_1a8 == 0) ((Material *)local_1f4)->depthMode = eDepthNormal;
  else if (local_1a8 == 1) ((Material *)local_1f4)->depthMode = eDepthNoWrite;
  else if (local_1a8 == 2) ((Material *)local_1f4)->depthMode = eDepthOff;
}
std::basic_istream<char,std::char_traits<char>_>::read(... propertyCount ...);
```

Therefore v4 omits the four-byte depth word and retains `eDepthNormal`, while
v5 and v6 serialize it.

## Scene-layout gates

`KN5IO::loadBinaryV2` at `0x1003b3d9` reads mesh material IDs and then gates
the later fields on `loadingVersion`:

```c
if (2 < this->loadingVersion) {
  std::basic_istream<char,std::char_traits<char>_>::read(...);
  std::basic_istream<char,std::char_traits<char>_>::read(...);
  std::basic_istream<char,std::char_traits<char>_>::read(...);
}
if (3 < this->loadingVersion) {
  std::basic_istream<char,std::char_traits<char>_>::read(...);
  std::basic_istream<char,std::char_traits<char>_>::read(...);
}
```

The first gate covers layer and LOD fields. The second covers the static-mesh
bounding sphere and renderable flag. Both are present in version 4, so its scene
records use the same layout consumed by the existing current node parser.
