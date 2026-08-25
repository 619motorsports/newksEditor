# Native BMP texture support

Status: the native texture planner decodes the observed Windows BMP font
layout to one owned RGBA8 level.

The installed editor contains these fixtures:

- `content/fonts/comic.bmp` is a 1024 x 32, 24-bit Windows BMP file. Its size
  is 98,358 bytes. Its SHA-256 is
  `789bc483fee57f77bdbf432c468a47b334b0ae1459649fdf8bbbb4d27d760ca4`.
- `content/fonts/default_big.bmp` is a 512 x 512, 24-bit Windows BMP file. Its
  size is 786,486 bytes. Its SHA-256 is
  `cef833b10b2b48b9f3891d6f38cfbe6a41edb0069ca0e1f5fbc94427991bfa1b`.

Both files use a 40-byte Windows info header. Their pixel offset is 54 bytes.
Their compression field is `BI_RGB`.

The native decoder accepts this 24-bit layout in bottom-up or top-down row
order. It converts BGR pixels to RGBA8 and makes each alpha value opaque. It
checks the header sizes, pixel offset, declared file size, row padding,
dimensions, input size, and output size before allocation.

The decoder rejects compressed, indexed, and 32-bit BMP layouts. These layouts
need separate fixture evidence before the native parser can accept them.
