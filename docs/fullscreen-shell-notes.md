# Fullscreen shell: what the decompile confirms, and one lever we are not using

Notes from reading `BZ1_Source/1.5` against `src/fullscreen_fix.cpp`. No
behaviour was changed on the strength of these; they need a pair of eyes on a
real screen.

## The premise in fullscreen_fix.cpp is correct

The header comment claims the shell is a GDI dialog kept visible under an
exclusive swap chain by `IDirect3DDevice9::SetDialogBoxMode`. That is exactly
what the code does. `D3D_Change_Mode_Ex` (`0x00543085`):

```c
if (ResolutionMode != 0)
    lpD3DDevice->SetDialogBoxMode(lpD3DDevice, 0);   // entering a mission
...
if (ResolutionMode == 0)
    lpD3DDevice->SetDialogBoxMode(lpD3DDevice, 1);   // returning to the shell
```

So the reasoning the four modes are built on holds up, and the diagnosis — that
modern WDDM drivers no longer composite the dialog — remains the right one.

## `ResolutionMode` is the signal we should be using

`ResolutionMode` (`0x00CD5B44`, a plain `int`) **is** the shell/mission flag.
Zero means the shell; any other value indexes `VideoMode` (`0x0062DA68`, stride
`0x20`, width at `+0x10` and height at `+0x14`) for the gameplay resolution.
`bzone.exe` is `RELOCS_STRIPPED` with no `DYNAMIC_BASE`, so that address is
stable — proven from the PE headers.

Today `ConvertExclusiveRequest` decides whether to mirror by comparing the
requested back buffer against the monitor:

```cpp
if (g_logicalW >= monitorW && g_logicalH >= monitorH)   // treat as a mission
```

That is a proxy, and it is wrong in both directions:

* A **mission** at any resolution below the desktop — 640x480, 1280x720 on a 4K
  panel — is routed through the DWM thumbnail. Mirroring gameplay costs a
  composition pass and at least a frame of latency for no benefit the display
  scaler would not have given for free.

* A **shell** on a machine whose desktop happens to be 640x480 would take the
  mission path and never mirror at all.

Reading `ResolutionMode` at `CreateDevice`/`Reset` time replaces the guess with
the game's own answer. `D3D_Change_Mode_Ex` assigns it before calling
`D3DAppIResetDevice`, so it is already current when our `Reset` hook runs —
worth confirming at runtime with a log line before relying on it.

The obvious shape: mirror only when `ResolutionMode == 0`, and give missions a
plain borderless window at the requested size. That also removes the awkward
case where the mirror is torn down and rebuilt on every mission entry and exit.

## Things worth checking before changing anything

* `Mode=mirror` is the shipped default in `bz15_shim.ini`, but the mirror
  destination is `hostClient` with `MirrorAspect=stretch`, so a 4:3 shell is
  stretched to 16:9 on a widescreen panel. `fit` pillarboxes correctly. Which of
  those is wanted is a taste call, not a bug, but the default stretches.

* `WindowProc` (`0x00478DF5`) branches on `ResolutionMode` in eight places for
  paint, sizing and activation. If any of the shim's window hooks fight it, that
  is where the symptoms would come from.

* `D3D_Change_Mode_Ex` calls `SetWindowLongA` and `SetWindowPos` itself, right
  after `D3DAppIResetDevice`. `HookReset` already re-pins the window afterwards
  for this reason; the same is not done around `GDI_ChangeMode`
  (`0x005053AF`), which also writes `ResolutionMode`.
