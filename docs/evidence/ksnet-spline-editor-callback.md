# ksEditor spline callback evidence

This note records recovered behavior from the installed `ksNet.dll` and its
matching PDB. The DLL SHA-256 is
`b38dcb826a3311d7233cf0a6a58e5da16b6c8679f8490091e7b434bf730091ca`.
It does not claim that the native viewport implements this pass.

`SplineEditor.onNodeRender` has managed token `0x06000107` and RVA `0x2F748`.
It reads `RenderContext.meshFilter.passID` and returns unless the pass is
`RenderPassID::Opaque`. It also returns when the primary spline is null.

The constructor has token `0x06000106` and RVA `0x2EB3C`. It stores the spline,
graphics object, screen message, and frame delta. It registers the callback on
the `SCENE_FINISHED` node through `NodeCallback.addListener` (`0x060002A8`).

The callback draws the primary raw spline in `(3, 0, 3, 1)`. If interpolated
display is active, it calls `renderSplineInterpolated` (`0x06000108`) instead.
That helper samples from zero through one with an increment of
`0.0001999999949975`. It sends the samples to `GLRenderer::spline`.

If the input and output interval values are valid, the callback draws that
interval in `(0, 0, 3, 1)`. The interval helper (`0x06000109`) uses the same
increment. It selects depth mode 2 before the draw and restores depth mode 0.

The callback creates a wrapped horizontal direction cache. It creates the left
and right splines only when the left spline exists and the right spline is
null. Enabled side splines use `(0, 3, 3, 1)` and the raw line-strip helper.

The callback then processes the selected indices and the current movable edit
point. If more than four edit points exist, it draws a temporary interpolating
spline in `(0, 3, 0, 1)`. It draws camber data only when that mode is active.

`GLRenderer::spline` is at `0x100479E8` and PDB location `0001:289256`.
It returns when the point count is two or less. Otherwise, it emits a line
strip in chunks that do not exceed the renderer vertex limit.

`NodeCallback::render` is at `0x1006096E`. It calls listeners in insertion
order before it renders the callback node. The scene order puts the model and
selected mesh before `SCENE_FINISHED`. The grid and axis children follow it.
