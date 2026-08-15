# I'm targeting a narrow patch, intentionally focusing on these: 
1. Menus not rendering at full screen resolution
2. In game HUD sprites not scaling with screen resolution
3. Menu AVI videos not playing properly on modern OS

<img width="3502" height="1970" alt="vlc_gl4Uk64G1v" src="https://github.com/user-attachments/assets/5d46f2a9-0242-4b05-b733-22363e0e6e24" />
<img width="3502" height="1970" alt="vlc_dCkU6TCsjO" src="https://github.com/user-attachments/assets/5228871c-9e7c-4b56-a9ac-76c6b606b9dd" />
<img width="3502" height="1970" alt="vlc_0JqR5nSoEL" src="https://github.com/user-attachments/assets/323318c7-574f-4696-bb76-4ff92e0f520c" />
<img width="3502" height="1970" alt="image" src="https://github.com/user-attachments/assets/5d94fd30-46b6-45bf-9d6d-e31f5a45ad2d" />

```
; Battlezone 1.5 shim settings.
; Place next to bzone.exe. Every key is optional; the default is shown.

[Fullscreen]
; Battlezone's shell (main menu, options, mission select) is a Win32 dialog
; drawn with GDI as a child of the game window, not a D3D surface. On affected
; modern Windows builds the exclusive D3D9 + SetDialogBoxMode path stops making
; that dialog visible. The exact Windows presentation-stack regression is not
; yet isolated; the fullscreen compatibility modes below bypass the broken path.
;
; mirror      : keep your desktop resolution. The game window stays 640x480 and
;               a fullscreen host window mirrors it with a DWM thumbnail (the
;               same mechanism as taskbar previews), scaled on the GPU and
;               aspect-corrected. Mouse input is mapped back into the game.
;               No monitor mode switching at all. Missions at your native
;               resolution bypass the mirror entirely.
; displaymode : take the device out of exclusive mode so the dialog is visible
;               again, and put the monitor into the resolution the game asked
;               for so your panel upscales it -- the same thing exclusive
;               fullscreen was doing implicitly. The shell fills the screen.
;               Costs a monitor mode switch entering and leaving a mission.
; center      : leave the desktop resolution alone and just centre a borderless
;               window. The menus are visible but stay physically 640x480.
;               No mode switching, so no monitor resync.
; off         : leave the game's presentation path stock. Pair this with
;               DiagnoseShell=on to collect passive root-cause logs.
Mode=mirror

; Passive diagnostics only; does not change D3D presentation or window state.
; For a clean stock-path capture use Mode=off + DiagnoseShell=on.
DiagnoseShell=off

; Only used by Mode=mirror.
; stretch : fill the screen. The 4:3 shell is stretched to 16:9.
; fit     : pillarbox to keep the shell's original 4:3 proportions.
MirrorAspect=stretch


[Hud]
Scale=5
   ; auto | 1 (stock) | 2..16
   ```
