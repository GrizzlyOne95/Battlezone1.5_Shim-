# Fullscreen shell: what the decompile confirms, what is still unknown, and how to measure it

Notes from reading `BZ1_Source/1.5` against `src/fullscreen_fix.cpp` and from the
new passive instrumentation in `src/shell_diag.cpp`.

## What is proven

Battlezone's shell is a GDI/Win32 dialog kept visible under the fullscreen D3D9
path by `IDirect3DDevice9::SetDialogBoxMode`. `D3D_Change_Mode_Ex`
(`0x00543085`) does exactly this:

```c
if (ResolutionMode != 0)
    lpD3DDevice->SetDialogBoxMode(lpD3DDevice, 0);   // entering a mission
...
if (ResolutionMode == 0)
    lpD3DDevice->SetDialogBoxMode(lpD3DDevice, 1);   // returning to the shell
```

The shell itself is not a D3D surface. Its controls are owner-drawn through GDI
into a child dialog. That explains why changing Present rectangles or scaling
the D3D back buffer cannot fix the missing menu.

The fullscreen compatibility fix also does **not** repair `SetDialogBoxMode`.
It bypasses the failing path by converting the exclusive request to a windowed
borderless device and then recreating the old fullscreen appearance either with
a display-mode switch or with a DWM thumbnail mirror.

## What is not proven

The previous notes attributed the regression directly to "modern WDDM drivers."
That is too specific for the evidence currently in hand.

The observed fact is narrower: on affected modern Windows systems, Battlezone's
exclusive D3D9 + GDI + `SetDialogBoxMode(TRUE)` shell path no longer produces a
visible menu, while changing the device to a windowed presentation makes the
same GDI dialog visible again. Historical testing places the break around the
Windows 10 1803 era, but the exact Windows presentation-stack change has not yet
been isolated.

That distinction matters because disabling the user-facing Fullscreen
Optimizations compatibility setting does not restore Battlezone's stock shell.
The responsible change may sit deeper in the D3D9/runtime/compositor/display
compatibility path rather than being the Fullscreen Optimizations policy itself.

## Passive stock-path diagnostics

`src/shell_diag.cpp` adds an opt-in observer that is intentionally separate from
`fullscreen_fix.cpp`. It can therefore run while the compatibility fix is off.

For the cleanest capture:

```ini
[Fullscreen]
Mode=off
DiagnoseShell=on
```

With those settings, the shim does **not** alter Battlezone's requested D3D9
presentation mode. The diagnostic hook records:

- the real Windows major/minor/build number via `RtlGetVersion`;
- whether DWM composition reports enabled;
- the requested and granted `D3DPRESENT_PARAMETERS`;
- `IDirect3DDevice9::SetDialogBoxMode(TRUE/FALSE)` calls and returned HRESULTs;
- the current swap-chain presentation parameters and display mode around each
  `SetDialogBoxMode` call;
- `Present` calls while dialog mode is requested, including timing and HRESULT;
- `BeginPaint` activity for the game window and descendants;
- the game window and child-dialog geometry, visibility, style/ex-style, class
  and title;
- three `GetPixel` samples from each enumerated child window's client DC.

The last three points are meant to separate "the shell never painted" from
"the shell painted successfully but its pixels never reached the displayed
fullscreen surface."

A particularly useful failure signature would look conceptually like:

```text
SetDialogBoxMode(1) -> D3D_OK
BeginPaint ...
child ... visible=1 ... dc=<valid samples>
Present#1 ... D3D_OK
Present#2 ... D3D_OK
<menu still invisible>
```

That would strongly narrow the regression to the final fullscreen composition /
presentation stage rather than Battlezone's shell logic or GDI drawing itself.

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

**Done.** `ConvertExclusiveRequest` now reads `ResolutionMode` and logs
`fullscreen: ResolutionMode=N (shell|mission) -> mirror|display mode` on every
CreateDevice/Reset, so the assumption above is checked by every run's log. A
value outside `0..64` falls back to the old size test. Missions below the
desktop resolution take the `displaymode` path (monitor switched to the mission
mode, real game window, real input); `MirrorMissions=mirror` restores the old
behaviour.

## Why mirroring a mission could leave input stuck

Four things in the mirror host combined badly once a mission was mirrored:

* **Nothing could reactivate the game by clicking.** The host is a topmost,
  monitor-sized `WS_EX_NOACTIVATE` window and answers `WM_MOUSEACTIVATE` with
  `MA_NOACTIVATE`. If anything took the foreground mid-match (Alt+Tab, an
  overlay, a launcher, a notification that grabs focus), every click the player
  made to get back landed on a window that refuses activation. Keyboard and the
  game's foreground-only input stayed with the other window. The host now hands
  activation back to the game window on click.

* **The host stayed topmost over other applications.** It now tracks the
  foreground window (`EVENT_SYSTEM_FOREGROUND`): topmost while the game is in
  front, dropped behind whatever the player switched to otherwise.

* **Captured mouse input was never mapped.** Buttons, list boxes and scroll bars
  call `SetCapture` on a click. From then on Windows delivers real mouse
  messages straight to that control, with the physical cursor position -- host
  space, not game space -- so drags and releases land somewhere in the 640x480
  corner. A thread-local `WH_GETMESSAGE` hook now remaps real mouse messages
  aimed at the game while one of its windows holds capture, and wheel messages,
  which go to the focus window rather than the window under the cursor.

* **Missions were never written for forwarded mouse messages.** The mission is
  pure D3D. The shell's input path (post a mapped `WM_*BUTTON*` to the child
  under the cursor) has no business driving it. Missions are no longer mirrored
  by default.

## Other ways to put the shell on screen

The shell is a fixed 640x480 GDI dialog. The options, roughly in order of how
much they would improve on the mirror:

* **Magnification API full-screen transform (`MagSetFullscreenTransform`).**
  Windows' own magnifier transform scales the whole desktop on the GPU, and the
  system maps mouse input through the transform itself, so no forwarding, no
  host window and no focus games would be needed: the dialog stays a normal
  window at the monitor origin and the player clicks it directly. Costs: the
  scale is uniform (so `fit` only, no `stretch`), the cursor is magnified too,
  the process must restore the transform on exit, deactivation and crash, and
  only one process can own the full-screen magnifier at a time (it conflicts
  with the accessibility Magnifier). This is the most promising next
  experiment.

* **Keep the DWM thumbnail, fix the edges (what the shim now does).** Works on
  every DWM system and supports `stretch`, but input is second-hand and needs
  the capture, wheel and focus handling above.

* **`displaymode`.** No mapping at all, the panel scales -- at the cost of a
  monitor resync on every shell/mission transition.

* **Rendering the dialog through D3D.** Capture the dialog each frame
  (`PrintWindow(PW_RENDERFULLCONTENT)`) into a texture and draw it scaled from a
  present loop of our own. Most control over filtering and aspect, but the
  shell's device presents only twice and then idles, so the shim would own a
  render loop and still has to map input exactly like the mirror. Not worth it
  over the Magnification option.

## Things worth checking before changing behaviour

* Run the passive diagnostic with `Mode=off` first. That gives us the stock
  failure without the fullscreen workaround changing the device model beneath
  it.

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
