# ksNet AI spline grid evidence

This note records the recovered native grid algorithm. The C++ point-edit path
must implement this algorithm before it changes point positions.

## Symbols and data layout

`AISpline::buildGrid` is at `0x10068a86`. The thunk adds `0x24` to `this` and
jumps to `InterpolatingSpline::buildGrid` at `0x10034e56`.

The PDB gives this 32-byte `GridData` layout:

- `maxExtreme` is at offset `0x00`.
- `minExtreme` is at offset `0x0c`.
- `samplingDensity` is at offset `0x18`.
- `neighborsConsideredNumber` is at offset `0x1c`.

`InterpolatingSpline::saveGrid` is at `0x10037294`. Its record order matches
the current C++ parser and writer.

## Recovered algorithm

The method first deletes the old grid. It then allocates new `GridData`.
The constructor sets `samplingDensity` to `10.0F`. It sets the neighbor count
to 10.

The method scans all 20-byte spline points. It calculates X and Z bounds only.
It subtracts `350.0F` from each minimum. It adds `350.0F` to each maximum.
These bound operations use scalar single-precision arithmetic.

The grid dimensions use these expressions:

```text
nx = int((maxX - minX) / samplingDensity)
nz = int((maxZ - minZ) / samplingDensity)
```

The positive float conversion truncates toward zero. Each X index creates one
outer row. Each Z index creates one cell in that row.

Each cell samples its center with double-precision intermediate values:

```text
x = float(double(minX) + (double(xIndex) + 0.5) * double(samplingDensity))
z = float(double(minZ) + (double(zIndex) + 0.5) * double(samplingDensity))
```

`Spline::closestPointIndicesFlat` is at `0x1003375d`. It scans all points.
The distance uses the X and Z components and ignores Y. The method uses
single-precision subtraction, multiplication, addition, and square root.

Each temporary record contains the source index and the distance. The method
uses `std::sort` with a distance-only comparator. It then copies 10 records.
The DLL links to the Visual C++ 2013 standard library. The inlined sort code
matches that library's median, partition, and insertion-sort implementation.

The C++ port includes this recovered sort implementation. This code gives the
same equal-distance order on all supported standard libraries.

The native maximum initializer contains the bit value `0x00800000`. This value
is a small positive float, not zero. The port must preserve this edge-case
behavior and label it as recovered native behavior.

## Fixture result and safety boundary

The checked-in `pit_lane_with_grid.ai` fixture has 4,329 points. Its grid has
185 X rows and 163 Z cells in each row. Each cell contains 10 indices.

The native method has no useful allocation bounds. It also reads outside the
temporary array when the spline has fewer than 10 points. An empty spline can
produce invalid dimensions from the initial extremes.

The C++ port rejects empty splines. It returns all available indices for a
spline with fewer than 10 points. This behavior is an intentional safety
difference. The grid metadata keeps the native neighbor count of 10.

The C++ port rejects non-finite coordinates and invalid dimensions. It bounds
rows, cells, indices, distance work, and memory before allocation.
