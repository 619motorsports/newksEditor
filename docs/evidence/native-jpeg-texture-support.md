# Native JPEG texture support

Status: bounded JPEG-to-RGBA8 planning is implemented for external texture
payloads when CMake finds a system JPEG decoder.

Asset evidence:

- Restored LFS asset: `test/content/cars/619_gen6_arca_base/skins/default/preview.jpg`
- The asset is baseline 8-bit RGB JPEG, 1022 x 575, 207,941 bytes.
- Installed SDK examples also include `sdk/dev/car_pipeline_2.0rev/Bitmaps/mat_mirror.jpg`.

The native decoded-texture planner previously recognized PNG bytes and treated
all other payloads as DDS. A JPEG external override therefore failed as an
invalid DDS. The new bounded path recognizes the JPEG SOI marker, requires an
EOI marker, enforces input, dimension, checked pixel-size, and output-byte
limits, and produces one owned RGBA8 level. CMake keeps the decoder optional;
builds without JPEG support return an explicit unsupported-format diagnostic.

The texture upload test exercises the restored LFS dimensions and output size,
short prefixes, a missing EOI, and the decoded-output budget.
