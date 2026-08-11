# Scaling the in-game HUD with resolution

Battlezone 1.5's cockpit HUD is authored in fixed 640x480 pixels. At 4K every
gauge, label, radar and reticle is drawn at its original pixel size in the
corner of the screen, roughly a fifth of its intended size. This is the plan for
fixing that from the shim.

Claims below are tagged **PROVEN** (verified against `bzone.exe` bytes or a live
run), **DECOMPILE** (read out of the Ghidra/PDB export in
`BZ1_Source/1.5`) or **ASSUMED** (not yet checked).

All addresses are absolute virtual addresses. `bzone.exe` 1.5.2.27 is
`RELOCS_STRIPPED`, has no `DYNAMIC_BASE` and carries no relocation directory, so
it always loads at `0x00400000` and these addresses are stable — **PROVEN** from
the PE headers. The shipped `bzint.pdb` resolves every symbol named here.

## How the HUD actually draws

`RenderHUD` (`0x0047834D`) is the whole 2D pass. It runs the cockpit, the radar,
`DisplayInterface::RenderAll`, the frame counter, the scoreboard and the network
overlay — **DECOMPILE**.

Every HUD widget is a `DisplayInterface` subclass with its own `Render()`:
`CockpitRadar`, `ScrapGauge`, `StatusDisplay`, `Reticle`, `TargetCam`,
`ControlPanel`, `SelectionDisplay`, `HardPoints`, `CockpitTimer`, `PathDisplay`,
`LauncherInterface`, `SniperInterface`, `MapRadar`. They all bottom out in the
same handful of 2D primitives.

The single most important one is `DrawSprite` (`0x004FBE30`):

```c
int DrawSprite(_GRAPHIC_BUFFER *buffer, int spriteIndex, int x, int y, int flags)
```

It looks the sprite up in `spriteTable` (`0x00CEDBA0`, 32 bytes per entry —
**PROVEN**, the index is scaled by `shl ebx,5`), then in the D3D path builds a
four-vertex quad whose screen rectangle is `(x, y)` to `(x + w, y + h)` and whose
UVs come from the same `w`/`h`. Depth is `1.0f / spriteZ` (`0x00CEDB80`), and
callers nudge `spriteZ` by `+0.015`, `+0.1`, `+0.2`, `+0.3` to layer panels
behind labels — **DECOMPILE**.

Two consequences matter:

* **The screen size and the UV extent are the same number.** You cannot make a
  HUD sprite bigger by editing `spriteTable[i].width`; that would stretch the
  atlas lookup by exactly as much as it stretched the quad, and the sprite would
  come out the same size with the wrong texels.

* **Text is sprites.** `Font_Print_String` (`0x004F409E`) resolves each character
  to a sprite index and calls `DrawSprite`, advancing `x` by
  `GetCharacterAddress(font, ch)->Width` — **DECOMPILE**. Anything that scales
  `DrawSprite` scales the font for free.

Positions are compile-time constants in globals — `SCRAP_PANEL_POS`,
`MODE_BUTTON_POS[]`, `MODE_BUTTON_RECT[]`, `CONTROL_PANEL_RECT`,
`RADAR_CENTER`, `CMD_BUTTON_POS`, `TITLE_BUTTON_POS` and so on. `AdjustPositions`
(`0x004C3A30`) is the only code that rewrites them and it only does so once, to
widen the menu gap for Japanese — **DECOMPILE**. There is no layout pass to hook.

A handful of places are already resolution-aware, and they are the ones that
complicate everything:

```c
// ScrapGauge::Render — a 320-mode layout switch
if (400 < Device.Viewport.Width) { ... }

// ControlPanel::Render — anchored to the bottom edge
CONNECTOR_POS.y = Device.Viewport.Height - ((Device.Viewport.Height < 480)
                                            ? Device.Viewport.Height * 180 / 480
                                            : 180);
```

`Device` lives at `0x00D423E0` and `Device.Viewport` is at offset 0 of it —
**PROVEN**: `DrawSprite` tests `(VIDEO_DEVICE *)buffer == &Device` on a
`_GRAPHIC_BUFFER *` argument.

## What does not work

**Editing the sprite table.** Ruled out above: width is both the blit size and
the UV extent.

**`DrawScaledSprite` (`0x004FC0DE`) as a drop-in.** It already does the right
geometry — explicit destination width/height, UVs taken separately from the
sprite record — but its depth argument is an `int` and it emits
`1.0f / (float)z`. The HUD layers itself with fractional `spriteZ` offsets as
small as `0.015`, which an integer depth collapses. Use it as the reference for
building the quad, not as the call target — **DECOMPILE**.

**Scaling positions only, at the draw call.** Mouse hit-testing runs
`InsideRect(DisplayInterface::mousePos, MODE_BUTTON_RECT[i])` against the
unscaled rect globals, so scaling the drawing alone breaks every click.

**Scaling the layout globals.** Some rectangles are not in globals at all.
`ControlPanel::Render` builds health-bar rects from inline literals
(`left = 10`, `right = 0x9C`) every frame. Chasing those is per-call-site work
across a 1500-line function.

## The proposal: a virtual 640x480 HUD space

Let the game keep believing it is laying out a 640x480 screen, and convert at the
two boundaries where the illusion meets reality.

* **Output boundary** — every 2D primitive multiplies incoming coordinates *and*
  extents by `S` before it draws.
* **Input boundary** — the mouse position is divided by `S` right after the game
  computes it, so hit-testing compares virtual against virtual.
* **Viewport boundary** — for the duration of the 2D pass only,
  `Device.Viewport.Width`/`Height`/`Pane` report the virtual size, so
  edge-anchored code like `CONNECTOR_POS.y` lands correctly in virtual space and
  is then scaled up like everything else.

The reason this is worth the trouble: **layout falls out for free.**
`Font_Print_String` accumulates its pen position in virtual pixels and hands the
final coordinate to `DrawSprite`, which scales it — so glyphs come out `S` times
larger *and* `S` times further apart with no font work at all. Same for
`ControlPanel`, which advances by `GetSpriteWidth(idx)` between a sprite and its
label. **`GetSpriteWidth`/`GetSpriteHeight` must therefore be left alone** —
they return virtual sizes, which is exactly what their callers want.

### Choosing S

```
S = max(1, floor(viewportHeight / 480))
```

Integer only, because these are hand-drawn pixel sprites and a fractional scale
with a bilinear filter will smear them. 1080p gives 2x, 1440p exactly 3x, 4K 4x.
An INI override (`[Hud] Scale=auto|1|2|3|4`) covers taste and ultrawide.

### The hook surface

| what | address | change |
|---|---|---|
| `RenderHUD` | `0x0047834D` | wrap: install virtual viewport, call original, restore |
| `DisplayInterface::SimulateAll` | `0x004C8584` | call original, then divide `mousePos` by `S` |
| `DrawSprite` | `0x004FBE30` | scale position and extent, emit the quad directly |
| `Font_Print_String` | `0x004F409E` | none needed — rides on `DrawSprite` |
| `Clipped_Rect_Filled` | `0x004F9780` | scale all four coordinates |
| `Graphic_Rect_Filled` | `0x004F97A1` | scale all four coordinates |
| `Graphic_Line` | `0x004F988D` | scale all four coordinates |
| `Clipped_Line` | `0x004F975F` | scale all four coordinates |
| `DrawHorizontalGauge` | `0x004C8FC3` | scale the rect |

Supporting globals, all **PROVEN** by cross-referencing the code bytes that read
them:

```
spriteTable                       0x00CEDBA0   (32-byte entries)
spriteTableSize                   0x00CFDBA0
spriteZ                           0x00CEDB80
Foreground_Color                  0x00CEDB84
colorTable                        0x0062D9A8
useD3D                            0x0062DA64
Device            (Viewport @ +0) 0x00D423E0
Default_Font                      0x00D423C0
hudFlags                          0x00626D80
Cockpit_Visible                   0x00626DF0
useDisplayInterface               0x00626DB0
DisplayInterface::currentBuffer   0x00B44520
DisplayInterface::mousePos        0x00B44514
Draw_D3D_Poly                     0x00540BEF
ClipSprite                        0x004FBD16
```

`mousePos` is computed in exactly one place, from the pane extent and a 16.16
fraction — **DECOMPILE**:

```c
mousePos.x = ((Pane.x1 - Pane.x0) + 1) * command_controls.cmd_x >> 16;
mousePos.y = ((Pane.y1 - Pane.y0) + 1) * command_controls.cmd_y >> 16;
```

so the input half is a two-line change.

### The `DrawSprite` replacement

Model it on the existing D3D branch and keep the flag semantics, which are
**DECOMPILE** but unambiguous:

```
0x000007  low bits index BitmapOper2PolyTypeD3D[] (blend mode)
0x010000  x -= w/2      (centre horizontally)
0x020000  x -= w        (right-align)          -- only when 0x010000 is clear
0x040000  y -= h/2      (centre vertically)
0x080000  y -= h        (bottom-align)         -- only when 0x040000 is clear
0x100000  swap u0/u1    (horizontal flip)
0x200000  offset by Pane.x0/y0 and clip via ClipSprite
```

Anchoring must be applied in virtual space *before* scaling, otherwise a
centred sprite drifts by `(S-1)*w/2`.

Clipping is the one place that must use the **real** pane, not the virtual one:
`ClipSprite` adjusts UVs as it trims, so it has to see the actual pixel
rectangle. Save the real `Pane` when installing the virtual viewport and use the
saved copy inside the primitive hooks.

## Known problems to solve, in order

1. **The software radar mesh.** When `D3RadarType == 1`, `RenderHUD` ends the D3D
   scene, locks the back buffer and calls `Render_RadarMesh` (`0x004BFCBE`) to
   draw the radar directly into raw pixels, then restarts the scene —
   **DECOMPILE**. That path does not go through the primitives above. Until it is
   handled the radar will stay unscaled while everything around it grows. Ship
   phase 1 with the radar excluded rather than half-scaled.

2. **The 320-mode branches.** `if (400 < Device.Viewport.Width)` gates whole
   panels. With a virtual viewport of, say, 640x540 at 4K these stay on the
   640 path, which is what we want — but it needs confirming rather than
   assuming, because a virtual width below 400 (very tall aspect, large `S`)
   would silently drop panels. Clamp the virtual width to >= 400.

3. **Filtering.** `Draw_D3D_Poly` inherits whatever sampler state is current.
   Point sampling keeps the sprites crisp at integer scales; bilinear will look
   soft and will bleed neighbouring atlas entries at the sprite edges, since HUD
   sprites are packed into a shared texture with no gutters. Force point
   sampling for the HUD pass — **ASSUMED** that the state is not reset between
   quads; verify.

4. **`spriteZ` and depth.** The scaled quads keep the original `1.0f / spriteZ`,
   so layering is unchanged. No action, but it is the thing most likely to be
   broken by a careless rewrite.

## Suggested phasing

**Phase 1 — prove the model on one widget.** Hook `DrawSprite` only, gate it on
a single sprite range or on `ScrapGauge`, and confirm the scrap/pilot readout
grows and stays legible. No viewport swap, no input change. This answers the only
question that really matters: does a scaled quad through `Draw_D3D_Poly` look
right.

**Phase 2 — whole 2D pass.** Add the `RenderHUD` viewport swap, the remaining
primitives, and the `mousePos` division. Test the build menu specifically: it is
the only HUD element with real mouse hit-testing, so it is the one that proves
the input boundary.

**Phase 3 — radar.** Either scale `Render_RadarMesh`'s raw-pixel output or
convert that path to the D3D primitives.

**Phase 4 — INI, and an opt-out.** `[Hud] Scale=` plus `Scale=1` meaning stock,
so a regression is one line away from being disproved.

## What was actually built

`src/hud_scale.cpp` implements this, and five things came out differently once
the decompile was checked against the executable's own bytes. Each is recorded
here because each one would have been a bug.

**`DrawSprite` is not reimplemented.** The stock function builds its quad and
the finished quad is scaled on the way out, in a `Draw_D3D_Poly` hook armed for
exactly the one call `DrawSprite` makes. Scaling a whole quad uniformly about
the origin scales position and extent together, so the anchor flags above are
preserved for free — a centred sprite stays centred rather than drifting by
`(S-1)*w/2` — and the anchoring, atlas UVs, `ClipSprite` and `spriteZ` depth
never have to be duplicated. `Draw_D3D_Poly` was verified not to clip: it only
calls `Set_Rounding` and dispatches through `Submit_D3D_TL_Vtltbl`.

**The hook scope is `DisplayInterface_RenderAll` (`0x004C870E`), not
`RenderHUD`.** `RenderHUD` opens with `Render_Entity_Cockpit`, which is
`Render_CachedBSP_Entity` on the cockpit model — real 3D geometry. Wrapping
`RenderHUD` in a virtual viewport would have changed the cockpit's projection.
`RenderHUD` also drives the software radar mesh between an `End_D3D_Scene` /
`Begin_D3D_Scene` pair, which a virtual viewport should not straddle.

**`TargetCam::Render` (`0x004DB7E6`) is excluded.** It is 3D — it calls
`Camera_Set_Window`, `Camera_Set_Matrix` and `D3DAppSetViewport` — *and* it
already sizes itself from the viewport at 3/4 width by 1/4 height, so it scales
with resolution on its own. It suspends the virtual viewport for its duration.

**Only the `Clipped_*` primitives are hooked.** The table above lists
`Clipped_Rect_Filled` and `Graphic_Rect_Filled`, and `Clipped_Line` and
`Graphic_Line`, but the `Graphic_*` pair clip against the pane and then tail
into the `Clipped_*` pair. Hooking all four would have scaled every rectangle
and line twice. `Graphic_Diamond` needs no hook of its own: it is four
`Graphic_Line` calls, so radar blips scale in position and size for free.

**`mousePos` is not divided.** `DisplayInterface::SimulateAll` (`0x004C8584`)
recomputes it from the render buffer's pane extent at the top of the call and
then hit-tests within the same call, so dividing after the call would have been
a frame too late and overwritten immediately. Wrapping `SimulateAll` in the
same virtual viewport makes `mousePos` come out in virtual pixels on its own.

**The projected reticle scales about its own anchor.** `Reticle::Render`
(`0x004D93C7`) does not lay the sight out in HUD coordinates. It projects the
weapon direction through `MainCam.Const_*` and `MainCam.Orig_*`, producing a
real screen-pixel aim point. Its `DrawSprite` quad therefore grows around that
point instead of being multiplied about the screen origin. This enlarges the
reticle and pitch ladder without moving where the weapon is aiming.

**The hardware radar wireframe scales a copy of its points.** The terrain mesh
is simulated while the virtual viewport is active but rendered earlier than
the ordinary widget pass. `Render_RadarMesh` (`0x004BFCBE`) now establishes the
same scaling scope, and `D3D_PolyLine` (`0x00545E39`) receives a temporary copy
whose integer coordinates are multiplied by `S`. The source mesh is never
changed, because doing so would multiply its coordinates again every frame.
The raw-framebuffer `D3RadarType=1` path remains stock.

### Offsets proven from instruction bytes

`Device.Viewport` field offsets were recovered from operands rather than
assumed:

| field | offset | proof |
|---|---|---|
| `Width` | +0 | `cmp dword ptr [0x00D423E0], 190h` at `0x004C4AA0` and six other sites — the `400 < Viewport.Width` branch |
| `Height` | +4 | `mov ecx,[0x00D423E4]; mov esi,1E0h; cmp ecx,esi` at `0x004C49A7` — ControlPanel's `Viewport.Height < 480` |
| `Pane.x0/y0/x1/y1` | +28/+32/+36/+40 | `DrawSprite`'s `ClipSprite` call at `0x004FBF80` passes `[edi+1Ch]`, `[edi+20h]`, `[edi+24h]`, `[edi+28h]` with `edi = &Device` |

`POINT_3D` is 24 bytes (`x, y, z, u, v, luma`), from the vertex stride in both
`DrawSprite` and `Clipped_HW_Rect_Filled`. `DisplayInterface::SimulateAll` is
`__cdecl(int, float)`, from the `59 59 C3` caller-side cleanup in its wrapper.
Every hook re-checks its target's prologue bytes at install time and leaves the
game stock if they do not match, so a different build cannot be corrupted.

### Still not scaled

The `D3RadarType=1` software radar mesh writes directly into a locked back
buffer, bypassing all hardware primitives, so it keeps its stock size. The
normal hardware wireframe and the rest of the radar (blips, compass,
connectors, and dish) scale together.

Everything drawn outside `DisplayInterface::RenderAll` also stays stock:
`Show_framerate`, `Scores_DisplayScores`, `Network_DisplayInfo` and
`GameFeature_RenderAll`.

## Verification

The shim log should record, once per mode change: real viewport, chosen `S`,
virtual viewport, and the clamped virtual width. A screenshot at 640x480 with
`Scale=1` and one at 4K with `Scale=4` should differ only in resolution — if any
element moves relative to its neighbours, an inline literal was missed.
