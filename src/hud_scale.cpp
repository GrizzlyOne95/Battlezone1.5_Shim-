// hud_scale.cpp
//
// Battlezone 1.5's cockpit HUD is authored in fixed 640x480 pixels. Every
// gauge, label, radar and reticle is drawn at its original pixel size, so at
// 4K the whole interface shrinks into a corner of the screen at roughly a
// fifth of its intended size. docs/hud-sprite-scaling-plan.md works out why
// from the decompile; this is the implementation of that plan.
//
// The core idea is a *virtual 640x480-ish HUD space*. The game keeps laying
// the HUD out for a small screen, and the illusion is converted to reality at
// two boundaries:
//
//   * Viewport boundary -- for the duration of the 2D widget pass only,
//     Device.Viewport reports the virtual size, so edge-anchored code such as
//     ControlPanel's `Device.Viewport.Height - 180` lands in virtual space.
//
//   * Output boundary -- every 2D primitive multiplies its coordinates *and*
//     its extents by S on the way to the hardware.
//
// Layout then falls out for free. Font_Print_String accumulates its pen
// position in virtual pixels and hands the result to DrawSprite, so glyphs
// come out S times larger *and* S times further apart with no font work at
// all; GetSpriteWidth keeps returning virtual sizes, which is exactly what
// its callers want.
//
// Why the sprite quad is scaled at Draw_D3D_Poly rather than in a rewritten
// DrawSprite: DrawSprite uses the sprite record's width for both the
// destination rectangle and the UV extent, so the sprite table cannot be
// edited, and reimplementing its D3D branch would mean duplicating the
// anchoring, the atlas UV maths, ClipSprite and the spriteZ depth. Instead
// the stock DrawSprite is allowed to build its quad exactly as it always
// did, and the finished quad is scaled on its way out. Scaling a whole quad
// uniformly about the origin scales position and size together, which
// preserves the centre/right/bottom anchor flags automatically -- a centred
// sprite stays centred instead of drifting by (S-1)*w/2.
//
// Draw_D3D_Poly is shared with the 3D world (ZSORTDraw), so the scaling is
// armed only for the single call the stock DrawSprite makes, never for world
// geometry.
//
// Addresses are absolute VAs into bzone.exe 1.5.2.27, which is
// RELOCS_STRIPPED with no DYNAMIC_BASE and so always loads at 0x00400000.
// Every one of them, and every structure offset, was verified against the
// instruction bytes in the shipped executable -- see the notes on each
// constant. Each hook additionally refuses to install unless the bytes at its
// target still match the prologue recorded here, so a different build is
// left stock rather than corrupted.
//
// Settings live in bz15_shim.ini next to bzone.exe:
//
//   [Hud]
//   Scale=auto   ; auto (default) | 1 (stock) | 2 | 3 | 4 ...

#include "hud_scale.h"
#include "shim_log.h"

#include <Windows.h>
#include <cstdint>
#include <cstdlib>
#include <cstring>

namespace
{
    // ---------------------------------------------------------------------
    // Game addresses
    // ---------------------------------------------------------------------

    // The 2D widget pass: DisplayInterface::RenderAll over every HUD widget
    // (CockpitRadar, ScrapGauge, StatusDisplay, Reticle, ControlPanel,
    // InfoDisplay, HardPoints, ...). Deliberately narrower than RenderHUD,
    // which also renders the 3D cockpit entity and drives the software radar
    // mesh through an End/Begin_D3D_Scene pair.
    constexpr uintptr_t kDisplayInterfaceRenderAll = 0x004C870E;

    // The matching simulate pass. It recomputes DisplayInterface::mousePos
    // from the render buffer's pane extent at the top of the call and then
    // hit-tests the widgets, so wrapping it in the same virtual viewport is
    // what keeps mouse hit-testing agreeing with what was drawn.
    constexpr uintptr_t kDisplayInterfaceSimulateAll = 0x004C8584;

    // TargetCam::Render is genuinely 3D -- it calls Camera_Set_Window,
    // Camera_Set_Matrix and D3DAppSetViewport to render a live model of the
    // current target -- and it already sizes itself from the viewport
    // (3/4 width by 1/4 height), so it scales with resolution on its own.
    // It is suspended out of the virtual viewport for its duration.
    constexpr uintptr_t kTargetCamRender = 0x004DB7E6;

    constexpr uintptr_t kDrawSprite = 0x004FBE30;
    constexpr uintptr_t kDrawD3DPoly = 0x00540BEF;

    // The reticle is not a laid-out HUD element. Reticle::Render projects it
    // through the camera --
    //   x = MainCam.Const_x * w * (...) + MainCam.Orig_x
    // -- so its position is already in real screen pixels and tracks where
    // the gun is pointing. Multiplying that position would throw it off the
    // screen; only its size should grow. It is scaled about its own anchor
    // point instead, and no virtual viewport is installed around it.
    constexpr uintptr_t kReticleRender = 0x004D93C7;

    // The radar terrain mesh. Its vertices are built in virtual pixels --
    // CockpitRadar::Simulate runs inside the virtual viewport -- but
    // RenderHUD draws it before the widget pass, where nothing scaled it, so
    // it landed at a fifth of the way into the screen. Wrapping it puts it
    // back under the same scale as the rest of the radar.
    //
    // The entry is a `mov ecx, &cockpitRadar / jmp CockpitRadar::RenderMesh`
    // thunk, so the five stolen bytes are exactly the mov.
    constexpr uintptr_t kRenderRadarMesh = 0x004BFCBE;

    // Radar_PolyLines emits the mesh through this; it takes an array of
    // tagPOINT and widens each one into the vertex buffer.
    constexpr uintptr_t kD3DPolyLine = 0x00545E39;

    // Selects RadarProc[D3RadarType & 7] in Draw_RadarMesh
    // (`mov eax,[0x00C913A0]; and eax,7; call [eax*4 + 0x0063115C]`).
    // Type 1 is Radar_FBW_16, which RenderHUD runs against a locked back
    // buffer in raw pixels -- nothing here can scale that, so it is skipped.
    constexpr uintptr_t kD3RadarType = 0x00C913A0;
    constexpr int kSoftwareRadarType = 1;

    // Upper bound on a single polyline run; longer runs are left unscaled
    // rather than risking a partial copy.
    constexpr long kMaxPolyLinePoints = 512;

    // Graphic_Rect_Filled (0x004F97A1) and Graphic_Line (0x004F988D) clip
    // against the pane and then tail into these two, so hooking only the
    // inner pair covers both entry points without scaling twice.
    constexpr uintptr_t kClippedRectFilled = 0x004F9780;
    constexpr uintptr_t kClippedLine = 0x004F975F;

    // VIDEO_DEVICE Device; its VIEWPORT/_GRAPHIC_BUFFER lives at offset 0.
    //
    // PROVEN from instruction operands:
    //   +0  Width   `cmp dword ptr [0x00D423E0], 190h` at 0x004C4AA0 and six
    //               other sites -- the `400 < Device.Viewport.Width` branch
    //               in ControlPanel/InfoDisplay/ScrapGauge/StatusDisplay.
    //   +4  Height  `mov ecx,[0x00D423E4]; mov esi,1E0h; cmp ecx,esi` at
    //               0x004C49A7 -- ControlPanel's `Viewport.Height < 480`.
    //   +28..+40    Pane.x0/y0/x1/y1, from DrawSprite's ClipSprite call at
    //               0x004FBF80, which pushes [edi+28h],[edi+24h] and passes
    //               [edi+1Ch],[edi+20h] with edi = &Device.
    constexpr uintptr_t kDevice = 0x00D423E0;
    constexpr size_t kViewportWidth = 0;
    constexpr size_t kViewportHeight = 4;
    constexpr size_t kPaneX0 = 28;
    constexpr size_t kPaneY0 = 32;
    constexpr size_t kPaneX1 = 36;
    constexpr size_t kPaneY1 = 40;

    // Non-zero when the Direct3D renderer is in use. The scaling only has a
    // path through hardware primitives; under the software rasteriser
    // DrawSprite does a straight bitmap copy that cannot be stretched here.
    constexpr uintptr_t kUseD3D = 0x0062DA64;

    // POINT_3D is 24 bytes: {float x, y, z; float u, v, luma;}. PROVEN from
    // the stack layout of both DrawSprite (vertices at ebp-0x7c, -0x64,
    // -0x4c, -0x34) and Clipped_HW_Rect_Filled (-0x304, -0x2ec, ...).
    constexpr size_t kPoint3DStride = 24;

    // The HUD is authored for this. S is chosen so the virtual viewport
    // stays as close to it as an integer divisor allows.
    constexpr int kAuthoredHeight = 480;

    // The 320-mode branches gate whole panels behind `400 < Viewport.Width`.
    // The virtual width must stay above this or panels silently vanish.
    constexpr int kMinVirtualWidth = 400;

    // ---------------------------------------------------------------------
    // Inline hook engine
    // ---------------------------------------------------------------------
    //
    // The existing shim fixes all hook imports, but the HUD lives entirely
    // inside bzone.exe, so these are 5-byte JMP detours with a trampoline
    // holding the displaced prologue. Every target below begins with
    // position-independent bytes (no E8/E9 relative operands within the
    // stolen range), so the displaced bytes can be copied verbatim.

    struct InlineHook
    {
        uintptr_t target = 0;
        size_t stolen = 0;
        uint8_t* trampoline = nullptr;
        uint8_t original[24] = {};
        bool installed = false;
    };

    bool WriteCode(void* address, const void* data, size_t size)
    {
        DWORD oldProtect = 0;
        if (!VirtualProtect(address, size, PAGE_EXECUTE_READWRITE, &oldProtect))
            return false;

        memcpy(address, data, size);

        DWORD ignored = 0;
        VirtualProtect(address, size, oldProtect, &ignored);
        FlushInstructionCache(GetCurrentProcess(), address, size);
        return true;
    }

    // Refuse to touch anything unless the bytes still look like the build
    // these addresses were read out of.
    bool PrologueMatches(uintptr_t target, const uint8_t* expected, size_t size, const char* name)
    {
        if (memcmp(reinterpret_cast<const void*>(target), expected, size) == 0)
            return true;

        const auto* actual = reinterpret_cast<const uint8_t*>(target);
        ShimLog("hud: %s at 0x%08X does not match the expected prologue"
                " (found %02X %02X %02X %02X %02X); not hooking",
                name, static_cast<unsigned>(target),
                actual[0], actual[1], actual[2], actual[3], actual[4]);
        return false;
    }

    bool InstallHook(
        InlineHook& hook,
        uintptr_t target,
        const void* detour,
        size_t stolen,
        const uint8_t* expected,
        const char* name)
    {
        if (stolen < 5 || stolen > sizeof(hook.original))
            return false;

        if (!PrologueMatches(target, expected, stolen, name))
            return false;

        hook.trampoline = static_cast<uint8_t*>(
            VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
        if (!hook.trampoline)
            return false;

        hook.target = target;
        hook.stolen = stolen;
        memcpy(hook.original, reinterpret_cast<const void*>(target), stolen);

        // trampoline: <displaced prologue> jmp target+stolen
        memcpy(hook.trampoline, reinterpret_cast<const void*>(target), stolen);
        hook.trampoline[stolen] = 0xE9;
        const auto back = static_cast<int32_t>(
            (target + stolen) - (reinterpret_cast<uintptr_t>(hook.trampoline) + stolen + 5));
        memcpy(hook.trampoline + stolen + 1, &back, sizeof(back));

        // target: jmp detour, padded with NOPs to the instruction boundary.
        uint8_t patch[sizeof(hook.original)];
        memset(patch, 0x90, stolen);
        patch[0] = 0xE9;
        const auto forward = static_cast<int32_t>(
            reinterpret_cast<uintptr_t>(detour) - (target + 5));
        memcpy(patch + 1, &forward, sizeof(forward));

        if (!WriteCode(reinterpret_cast<void*>(target), patch, stolen))
        {
            VirtualFree(hook.trampoline, 0, MEM_RELEASE);
            hook.trampoline = nullptr;
            return false;
        }

        hook.installed = true;
        return true;
    }

    void RemoveHook(InlineHook& hook)
    {
        if (!hook.installed)
            return;

        WriteCode(reinterpret_cast<void*>(hook.target), hook.original, hook.stolen);
        hook.installed = false;

        if (hook.trampoline)
        {
            VirtualFree(hook.trampoline, 0, MEM_RELEASE);
            hook.trampoline = nullptr;
        }
    }

    // ---------------------------------------------------------------------
    // State
    // ---------------------------------------------------------------------

    using RenderAllFn = void(__cdecl*)();
    using SimulateAllFn = void(__cdecl*)(int, float);
    using TargetCamRenderFn = void(__fastcall*)(void*, void*);
    using DrawSpriteFn = int(__cdecl*)(void*, int, int, int, int);
    using DrawD3DPolyFn = void(__cdecl*)(void*, long, void*, long);
    using PrimitiveFn = void(__cdecl*)(void*, long, long, long, long, long, long);
    using ReticleRenderFn = void(__fastcall*)(void*, void*);
    using RenderRadarMeshFn = void(__cdecl*)();
    using PolyLineFn = void(__cdecl*)(POINT*, long, void*);

    InlineHook g_renderAllHook;
    InlineHook g_simulateAllHook;
    InlineHook g_targetCamHook;
    InlineHook g_drawSpriteHook;
    InlineHook g_drawPolyHook;
    InlineHook g_rectHook;
    InlineHook g_lineHook;
    InlineHook g_reticleHook;
    InlineHook g_radarMeshHook;
    InlineHook g_polyLineHook;

    // 0 means "auto".
    int g_configuredScale = 0;

    struct SavedViewport
    {
        int width = 0;
        int height = 0;
        int x0 = 0;
        int y0 = 0;
        int x1 = 0;
        int y1 = 0;
    };

    // Real viewport saved while the virtual one is installed.
    SavedViewport g_realViewport;

    // Chosen S. Kept across passes rather than cleared on the way out,
    // because the reticle is drawn outside them and still needs it.
    int g_scale = 1;
    int g_depth = 0;    // 2D pass nesting (RenderAll / SimulateAll / radar)
    int g_suspend = 0;  // >0 inside TargetCam, virtual viewport withdrawn
    bool g_viewportInstalled = false;
    int g_reticleDepth = 0; // >0 inside Reticle::Render
    int g_radarDepth = 0;   // >0 inside Render_RadarMesh
    int g_loggedScale = 0;  // last configuration written to the log
    int g_loggedHeight = 0;

    // How the next quad out of DrawSprite should be scaled. Armed for
    // exactly one Draw_D3D_Poly call by the DrawSprite hook.
    enum class QuadScale
    {
        None,
        // Multiply the corners. Position and extent grow together, which is
        // what a HUD element laid out in virtual pixels wants.
        Origin,
        // Grow about the anchor point, leaving the position alone. For
        // sprites the game has already placed in real screen pixels.
        Anchor,
    };

    thread_local QuadScale t_quadScale = QuadScale::None;
    thread_local float t_anchorX = 0.0f;
    thread_local float t_anchorY = 0.0f;

    int& DeviceField(size_t offset)
    {
        return *reinterpret_cast<int*>(kDevice + offset);
    }

    bool UsingD3D()
    {
        return *reinterpret_cast<const int*>(kUseD3D) != 0;
    }

    // Scaling is live only inside the 2D pass and outside TargetCam.
    bool Scaling()
    {
        return g_depth > 0 && g_suspend == 0 && g_viewportInstalled;
    }

    // DrawSprite, Clipped_Rect_Filled and Clipped_Line all pick their
    // hardware path with the same test -- `buffer == &Device && useD3D` --
    // and only that path can be scaled. Gating on it keeps the three
    // primitives consistent with each other: if the HUD ever renders into an
    // offscreen buffer instead, every one of them declines to scale and the
    // pass comes out stock rather than drawing scaled coordinates into a
    // small buffer.
    bool ScalingBuffer(const void* buffer)
    {
        return Scaling() && reinterpret_cast<uintptr_t>(buffer) == kDevice && UsingD3D();
    }

    int ChooseScale(int realWidth, int realHeight)
    {
        if (realWidth <= 0 || realHeight <= 0)
            return 1;

        // Integer only: these are hand-drawn pixel sprites, and a fractional
        // scale under a bilinear filter smears them and bleeds neighbouring
        // atlas entries, which are packed with no gutters.
        int scale = g_configuredScale > 0 ? g_configuredScale : realHeight / kAuthoredHeight;

        // Never let the virtual width fall into the game's 320-mode branches.
        const int widthLimit = realWidth / kMinVirtualWidth;
        if (scale > widthLimit)
            scale = widthLimit;

        return scale < 1 ? 1 : scale;
    }

    void InstallVirtualViewport()
    {
        g_realViewport.width = DeviceField(kViewportWidth);
        g_realViewport.height = DeviceField(kViewportHeight);
        g_realViewport.x0 = DeviceField(kPaneX0);
        g_realViewport.y0 = DeviceField(kPaneY0);
        g_realViewport.x1 = DeviceField(kPaneX1);
        g_realViewport.y1 = DeviceField(kPaneY1);

        g_scale = ChooseScale(g_realViewport.width, g_realViewport.height);
        if (g_scale <= 1)
            return;

        g_viewportInstalled = true;
        DeviceField(kViewportWidth) = g_realViewport.width / g_scale;
        DeviceField(kViewportHeight) = g_realViewport.height / g_scale;
        DeviceField(kPaneX0) = g_realViewport.x0 / g_scale;
        DeviceField(kPaneY0) = g_realViewport.y0 / g_scale;
        DeviceField(kPaneX1) = g_realViewport.x1 / g_scale;
        DeviceField(kPaneY1) = g_realViewport.y1 / g_scale;

        if (g_scale != g_loggedScale || g_realViewport.height != g_loggedHeight)
        {
            g_loggedScale = g_scale;
            g_loggedHeight = g_realViewport.height;
            ShimLog("hud: real viewport %dx%d, scale %dx, virtual viewport %dx%d",
                    g_realViewport.width, g_realViewport.height, g_scale,
                    DeviceField(kViewportWidth), DeviceField(kViewportHeight));
        }
    }

    void RestoreRealViewport()
    {
        DeviceField(kViewportWidth) = g_realViewport.width;
        DeviceField(kViewportHeight) = g_realViewport.height;
        DeviceField(kPaneX0) = g_realViewport.x0;
        DeviceField(kPaneY0) = g_realViewport.y0;
        DeviceField(kPaneX1) = g_realViewport.x1;
        DeviceField(kPaneY1) = g_realViewport.y1;
    }

    void ReinstallVirtualViewport()
    {
        if (g_scale <= 1)
            return;

        DeviceField(kViewportWidth) = g_realViewport.width / g_scale;
        DeviceField(kViewportHeight) = g_realViewport.height / g_scale;
        DeviceField(kPaneX0) = g_realViewport.x0 / g_scale;
        DeviceField(kPaneY0) = g_realViewport.y0 / g_scale;
        DeviceField(kPaneX1) = g_realViewport.x1 / g_scale;
        DeviceField(kPaneY1) = g_realViewport.y1 / g_scale;
    }

    // Enters the 2D pass. Returns false when the pass should run stock,
    // in which case no viewport was touched.
    bool EnterHudPass()
    {
        if (g_depth++ > 0)
            return g_viewportInstalled;

        if (!UsingD3D())
            return false;

        InstallVirtualViewport();
        return g_viewportInstalled;
    }

    void LeaveHudPass()
    {
        if (--g_depth == 0 && g_viewportInstalled)
        {
            RestoreRealViewport();
            g_viewportInstalled = false;
        }
    }

    // ---------------------------------------------------------------------
    // Detours
    // ---------------------------------------------------------------------

    void __cdecl Detour_RenderAll()
    {
        EnterHudPass();
        reinterpret_cast<RenderAllFn>(g_renderAllHook.trampoline)();
        LeaveHudPass();
    }

    void __cdecl Detour_SimulateAll(int view, float elapsed)
    {
        // DisplayInterface::mousePos is recomputed here from the render
        // buffer's pane extent and then hit-tested against the unscaled
        // 640x480 rect globals, so the virtual viewport has to be in place
        // for the whole call rather than divided out afterwards.
        EnterHudPass();
        reinterpret_cast<SimulateAllFn>(g_simulateAllHook.trampoline)(view, elapsed);
        LeaveHudPass();
    }

    // __thiscall: `this` arrives in ECX, which __fastcall's first argument
    // also occupies. The second is a placeholder for EDX, which the original
    // never reads.
    void __fastcall Detour_TargetCamRender(void* self, void* unused)
    {
        const bool active = g_depth > 0 && g_scale > 1 && g_suspend == 0;
        if (active)
        {
            RestoreRealViewport();
            ++g_suspend;
        }

        reinterpret_cast<TargetCamRenderFn>(g_targetCamHook.trampoline)(self, unused);

        if (active)
        {
            --g_suspend;
            ReinstallVirtualViewport();
        }
    }

    int __cdecl Detour_DrawSprite(void* buffer, int spriteIndex, int x, int y, int flags)
    {
        QuadScale mode = QuadScale::None;
        if (ScalingBuffer(buffer))
        {
            mode = QuadScale::Origin;
        }
        else if (g_reticleDepth > 0 && g_scale > 1 && UsingD3D())
        {
            // Reticle::Render projects the sight into real screen pixels.
            // Keep that projected anchor fixed and grow the sprite around it.
            mode = QuadScale::Anchor;
        }

        if (mode == QuadScale::None)
            return reinterpret_cast<DrawSpriteFn>(g_drawSpriteHook.trampoline)(
                buffer, spriteIndex, x, y, flags);

        // Let the stock code do the sprite lookup, the anchor flags, the
        // atlas UVs, ClipSprite and the spriteZ depth, then scale the quad
        // it produces.
        t_quadScale = mode;
        t_anchorX = static_cast<float>(x);
        t_anchorY = static_cast<float>(y);
        if ((flags & 0x200000) != 0)
        {
            t_anchorX += static_cast<float>(DeviceField(kPaneX0));
            t_anchorY += static_cast<float>(DeviceField(kPaneY0));
        }

        const int result = reinterpret_cast<DrawSpriteFn>(g_drawSpriteHook.trampoline)(
            buffer, spriteIndex, x, y, flags);
        t_quadScale = QuadScale::None;
        return result;
    }

    void __cdecl Detour_DrawD3DPoly(void* vertices, long count, void* skin, long type)
    {
        const QuadScale mode = t_quadScale;
        if (mode != QuadScale::None && vertices && count > 0)
        {
            t_quadScale = QuadScale::None;

            const auto factor = static_cast<float>(g_scale);
            auto* vertex = static_cast<uint8_t*>(vertices);
            for (long i = 0; i < count; ++i, vertex += kPoint3DStride)
            {
                auto* xy = reinterpret_cast<float*>(vertex);
                if (mode == QuadScale::Origin)
                {
                    // Position and extent grow together. The pane was
                    // virtualised as real/S, so this returns both to real
                    // pixels while preserving the stock anchor flags.
                    xy[0] *= factor;
                    xy[1] *= factor;
                }
                else
                {
                    // The reticle's anchor came from the real camera
                    // projection. Grow its already-anchored quad without
                    // moving the aim point.
                    xy[0] = t_anchorX + (xy[0] - t_anchorX) * factor;
                    xy[1] = t_anchorY + (xy[1] - t_anchorY) * factor;
                }
            }
        }

        reinterpret_cast<DrawD3DPolyFn>(g_drawPolyHook.trampoline)(vertices, count, skin, type);
    }

    void __cdecl Detour_ClippedRectFilled(
        void* buffer, long x0, long y0, long x1, long y1, long color, long oper)
    {
        if (ScalingBuffer(buffer))
        {
            // x1/y1 are inclusive -- Clipped_HW_Rect_Filled extends them by
            // one to build its quad -- so the far edge is scaled as the
            // exclusive edge and pulled back, keeping filled widths exactly
            // proportional instead of losing S-1 pixels per rectangle.
            const long scale = g_scale;
            x0 *= scale;
            y0 *= scale;
            x1 = (x1 + 1) * scale - 1;
            y1 = (y1 + 1) * scale - 1;
        }

        reinterpret_cast<PrimitiveFn>(g_rectHook.trampoline)(
            buffer, x0, y0, x1, y1, color, oper);
    }

    void __cdecl Detour_ClippedLine(
        void* buffer, long x0, long y0, long x1, long y1, long color, long oper)
    {
        if (ScalingBuffer(buffer))
        {
            const long scale = g_scale;
            x0 *= scale;
            y0 *= scale;
            x1 *= scale;
            y1 *= scale;
        }

        reinterpret_cast<PrimitiveFn>(g_lineHook.trampoline)(
            buffer, x0, y0, x1, y1, color, oper);
    }

    void __fastcall Detour_ReticleRender(void* self, void* unused)
    {
        ++g_reticleDepth;
        reinterpret_cast<ReticleRenderFn>(g_reticleHook.trampoline)(self, unused);
        --g_reticleDepth;
    }

    void __cdecl Detour_RenderRadarMesh()
    {
        // Radar_FBW_16 writes directly into a locked software back buffer.
        // Its pixels cannot be stretched by the hardware primitive hooks.
        if ((*reinterpret_cast<const int*>(kD3RadarType) & 7) == kSoftwareRadarType)
        {
            reinterpret_cast<RenderRadarMeshFn>(g_radarMeshHook.trampoline)();
            return;
        }

        const bool scaled = EnterHudPass();
        if (scaled)
            ++g_radarDepth;

        reinterpret_cast<RenderRadarMeshFn>(g_radarMeshHook.trampoline)();

        if (scaled)
            --g_radarDepth;
        LeaveHudPass();
    }

    void __cdecl Detour_D3DPolyLine(POINT* points, long count, void* skin)
    {
        if (g_radarDepth <= 0 || g_scale <= 1 || !points || count <= 0 ||
            count > kMaxPolyLinePoints)
        {
            reinterpret_cast<PolyLineFn>(g_polyLineHook.trampoline)(points, count, skin);
            return;
        }

        // Radar_PolyLines sometimes points into the persistent mesh and
        // sometimes into a temporary column. Never alter either source: the
        // persistent vertices are reused and would otherwise grow again on
        // every frame.
        POINT scaledPoints[kMaxPolyLinePoints];
        const long scale = g_scale;
        for (long i = 0; i < count; ++i)
        {
            scaledPoints[i].x = points[i].x * scale;
            scaledPoints[i].y = points[i].y * scale;
        }

        reinterpret_cast<PolyLineFn>(g_polyLineHook.trampoline)(scaledPoints, count, skin);
    }

    // ---------------------------------------------------------------------
    // Settings
    // ---------------------------------------------------------------------

    void BuildIniPath(char (&path)[MAX_PATH])
    {
        path[0] = '\0';
        if (!GetModuleFileNameA(nullptr, path, MAX_PATH))
            return;

        char* slash = strrchr(path, '\\');
        if (!slash)
        {
            path[0] = '\0';
            return;
        }

        slash[1] = '\0';
        strcat_s(path, "bz15_shim.ini");
    }

    // Returns false when the player has asked for stock behaviour.
    bool LoadSettings()
    {
        char ini[MAX_PATH] = {};
        BuildIniPath(ini);
        if (!ini[0])
            return false;

        char value[32] = {};
        GetPrivateProfileStringA("Hud", "Scale", "auto", value, sizeof(value), ini);

        if (_stricmp(value, "off") == 0 || _stricmp(value, "stock") == 0)
        {
            ShimLog("hud: scaling disabled by bz15_shim.ini");
            return false;
        }

        if (_stricmp(value, "auto") == 0)
        {
            g_configuredScale = 0;
            ShimLog("hud: scale=auto (%s)", ini);
            return true;
        }

        const int requested = atoi(value);
        if (requested == 1)
        {
            ShimLog("hud: scale=1, leaving the HUD stock");
            return false;
        }

        if (requested < 1 || requested > 16)
        {
            ShimLog("hud: scale=%s is not a value between 1 and 16; using auto", value);
            g_configuredScale = 0;
            return true;
        }

        g_configuredScale = requested;
        ShimLog("hud: scale=%d (%s)", g_configuredScale, ini);
        return true;
    }
}

bool InstallHudScale()
{
    if (!LoadSettings())
        return false;

    // Prologues as they appear in bzone.exe 1.5.2.27. Each stolen range ends
    // on an instruction boundary and contains no relative operands.
    //   RenderAll     push dword ptr [0x00626E00]
    //   SimulateAll   push ebp / mov ebp,esp / sub esp,10h
    //   TargetCam     push ebp / lea ebp,[esp-78h]
    //   DrawSprite    push ebp / lea ebp,[esp-64h]
    //   Draw_D3D_Poly push esi / mov ecx,300h
    //   rect + line   push ebp / mov ebp,esp / cmp dword ptr [ebp+8],0D423E0h
    //   Reticle       push ebp / lea ebp,[esp-78h]
    //   radar mesh    mov ecx,0B22810h
    //   D3D_PolyLine  push ebp / mov ebp,esp / push ecx / push ecx
    static const uint8_t kRenderAllPrologue[] = { 0xFF, 0x35, 0x00, 0x6E, 0x62, 0x00 };
    static const uint8_t kSimulateAllPrologue[] = { 0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x10 };
    static const uint8_t kTargetCamPrologue[] = { 0x55, 0x8D, 0x6C, 0x24, 0x88 };
    static const uint8_t kDrawSpritePrologue[] = { 0x55, 0x8D, 0x6C, 0x24, 0x9C };
    static const uint8_t kDrawPolyPrologue[] = { 0x56, 0xB9, 0x00, 0x03, 0x00, 0x00 };
    static const uint8_t kPrimitivePrologue[] =
        { 0x55, 0x8B, 0xEC, 0x81, 0x7D, 0x08, 0xE0, 0x23, 0xD4, 0x00 };
    static const uint8_t kReticlePrologue[] = { 0x55, 0x8D, 0x6C, 0x24, 0x88 };
    static const uint8_t kRadarMeshPrologue[] = { 0xB9, 0x10, 0x28, 0xB2, 0x00 };
    static const uint8_t kPolyLinePrologue[] = { 0x55, 0x8B, 0xEC, 0x51, 0x51 };

    struct Target
    {
        InlineHook& hook;
        uintptr_t address;
        const void* detour;
        size_t stolen;
        const uint8_t* prologue;
        const char* name;
    };

    const Target targets[] = {
        { g_renderAllHook, kDisplayInterfaceRenderAll, &Detour_RenderAll,
          sizeof(kRenderAllPrologue), kRenderAllPrologue, "DisplayInterface_RenderAll" },
        { g_simulateAllHook, kDisplayInterfaceSimulateAll, &Detour_SimulateAll,
          sizeof(kSimulateAllPrologue), kSimulateAllPrologue, "DisplayInterface::SimulateAll" },
        { g_targetCamHook, kTargetCamRender, &Detour_TargetCamRender,
          sizeof(kTargetCamPrologue), kTargetCamPrologue, "TargetCam::Render" },
        { g_drawSpriteHook, kDrawSprite, &Detour_DrawSprite,
          sizeof(kDrawSpritePrologue), kDrawSpritePrologue, "DrawSprite" },
        { g_drawPolyHook, kDrawD3DPoly, &Detour_DrawD3DPoly,
          sizeof(kDrawPolyPrologue), kDrawPolyPrologue, "Draw_D3D_Poly" },
        { g_rectHook, kClippedRectFilled, &Detour_ClippedRectFilled,
          sizeof(kPrimitivePrologue), kPrimitivePrologue, "Clipped_Rect_Filled" },
        { g_lineHook, kClippedLine, &Detour_ClippedLine,
          sizeof(kPrimitivePrologue), kPrimitivePrologue, "Clipped_Line" },
        { g_reticleHook, kReticleRender, &Detour_ReticleRender,
          sizeof(kReticlePrologue), kReticlePrologue, "Reticle::Render" },
        { g_radarMeshHook, kRenderRadarMesh, &Detour_RenderRadarMesh,
          sizeof(kRadarMeshPrologue), kRadarMeshPrologue, "Render_RadarMesh" },
        { g_polyLineHook, kD3DPolyLine, &Detour_D3DPolyLine,
          sizeof(kPolyLinePrologue), kPolyLinePrologue, "D3D_PolyLine" },
    };

    for (const Target& target : targets)
    {
        if (InstallHook(target.hook, target.address, target.detour,
                        target.stolen, target.prologue, target.name))
        {
            continue;
        }

        ShimLog("hud: could not hook %s; reverting to stock HUD", target.name);
        ShutdownHudScale();
        return false;
    }

    return true;
}

void ShutdownHudScale()
{
    // Unhook the outer passes first so no detour is executing beneath us.
    RemoveHook(g_simulateAllHook);
    RemoveHook(g_renderAllHook);
    RemoveHook(g_targetCamHook);
    RemoveHook(g_drawSpriteHook);
    RemoveHook(g_drawPolyHook);
    RemoveHook(g_rectHook);
    RemoveHook(g_lineHook);
    RemoveHook(g_reticleHook);
    RemoveHook(g_radarMeshHook);
    RemoveHook(g_polyLineHook);

    g_depth = 0;
    g_suspend = 0;
    g_viewportInstalled = false;
    g_reticleDepth = 0;
    g_radarDepth = 0;
    t_quadScale = QuadScale::None;
    g_scale = 1;
}
