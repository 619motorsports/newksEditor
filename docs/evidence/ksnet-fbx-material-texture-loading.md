# ksEditor FBX material and texture loading

## Scope

This evidence separates native ksEditor FBX behavior from the Apex WebGL
compatibility path. The addresses refer to `ksNet.dll` from the installed SDK.

## Native material values

`FBXImporter::load` is at `0x10004200`. It creates a `ksPerPixel` material and
sets `ksSpecularEXP` to one. It reads the first component of each FBX ambient,
diffuse, and specular color.

The importer calls `Material::setVar(float)` at `0x100408B6` for these values.
`MaterialVar::set` at `0x1003E10C` sends each value to `ShaderVariable::set` at
`0x1004985D`. The FBX color path does not call the four-component overload at
`0x10040890`.

## Native texture lookup

The importer reads the first eight entries in
`FbxLayerElement::sTextureChannelNames`. These entries end at
`SpecularFactor`. The loop adds four to its byte offset and stops at `0x20`.
The relevant instructions are at `0x10004BE8`, `0x10004BF0`, `0x1000507B`,
and `0x10005086`.

The importer accepts `FbxFileTexture` objects and converts each path to a
basename. It then searches the configured folders and the sibling `texture`
folder.

The native order is `DiffuseColor`, `DiffuseFactor`, `EmissiveColor`,
`EmissiveFactor`, `AmbientColor`, `AmbientFactor`, `SpecularColor`, and
`SpecularFactor`. The loop does not reach `NormalMap` or later SDK entries.

The first file that exists fills `txDiffuse`. Later channels do not replace
this resource. The importer does not request `txNormal`.

`Material::setTexture` at `0x1004082B` binds the texture handle and name. It
does not generate texture bytes. `ResourceStore::getTextureFromBuffer` at
`0x10041300` receives embedded KN5 bytes from `KN5IO::loadTexture`. No FBX call
reaches this buffer path.

`MaterialResource::MaterialResource` at `0x1003FDAF` initializes a null texture
handle, an empty file name, a resource name, and an integer slot. A missing
texture remains null. `GraphicsManager::setTexture` at `0x10046927` binds this
null resource instead of creating a fallback texture.

`Texture::Texture(buffer,size)` at `0x1003F7A2` forwards supplied bytes to
`kglCreateTextureFromBuffer` at `0x1000CA30`. `KGLTexture::KGLTexture(buffer)`
at `0x10013740` passes those bytes to Direct3D. These functions do not create a
one-pixel texture.

The DLL imports Direct3D image decoders for memory and files. It does not import
`D3D11CreateTexture2D`. Thus, the inspected texture path only consumes encoded
image bytes.

`kglCreateSampler` at `0x1000C560` maps filter value one to linear filtering.
It maps wrap value zero to repeat addressing. The solid-color compatibility
path uses the same linear and repeat sampler contract in the C++ renderer.

## Compatibility boundary

`src/fbx-import.js` creates a 132-byte legacy BGRA8 DDS when an FBX material
color has no image. `public/app.js` uses the same channel clamp and rounding
for CSP color resources. The C++ effective-scene bridge uses this exact source
contract for Vulkan and D3D12.

This synthetic texture is WebGL and CSP compatibility behavior. It is not
recovered ksEditor FBX behavior.

## Native port boundary

The C++ FBX converter retains bounded, material-scoped file-texture candidates.
Each candidate contains the texture object, material, channel, and source basename.
The converter does not read the referenced file or create image bytes.

The converter applies the recovered eight-channel order. It retains source
order within one channel. A later application adapter must resolve each
basename through an explicit `AssetSource` grant. Direct native FBX image
loading and viewport rendering remain unsupported.
