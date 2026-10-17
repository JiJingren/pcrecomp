/*
 * smoke.c - the win32hle port's own end-to-end test, and the first thing that
 * runs as an .ipa.
 *
 * There is no lifted Red Alert 2 to run yet: the lift needs the game's own
 * gamemd.exe, and until that exists this file is what proves the port works.
 * It is deliberately not a "hello world": it drives the same machinery the game
 * will drive, through the same shim ABI a lifted PE uses, so every layer that
 * had to be replaced for iOS is exercised.
 *
 *   user32    a real window (RegisterClassA / CreateWindowExA) and a real
 *             PeekMessage-style loop, so the pump hook -- where screen_ios.c
 *             presents a frame and drains touches -- is driven the way a lifted
 *             message loop drives it on Linux.
 *   ddraw     SetDisplayMode + a primary surface, drawn into through Lock/Unlock
 *             at 16 bpp, so screen_ios.c's RGB565 -> RGBA8888 conversion is
 *             what the picture comes from.
 *   gdi32     a DC on that surface (GetDC) and CoreText-rendered TextOutA, so
 *             gdidc_ios.c's font layer is checked, not just its raster ops.
 *   dsound    DirectSoundCreate + a looping 16-bit stereo secondary buffer with a
 *             triangle wave in it, so dsound_ios.c's RemoteIO mixer runs (or, on
 *             a device with no audio session, says so rather than crashing).
 *
 * The window procedure and the test itself are written as *lifted* functions and
 * registered in recomp_dispatch_table, exactly as the generated lift will be:
 * that is the only way user32 can call a WndProc (it goes through
 * hle_call_guest), and it keeps the test honest about the register file, the
 * guest stack and the stdcall argument convention.
 *
 * The whole thing runs on a worker thread (ios/main.m starts it there), because
 * iOS only delivers touches on the main thread; the guest owns its message loop
 * here, exactly as it will on Linux.
 */
#define RECOMP_GENERATED_CODE
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "win32hle.h"
#include "screen_ios.h"
#include "host.h"

/* ---- Win32 / DirectX / DirectSound constants we name explicitly ---- */
#define WM_DESTROY        0x0002u
#define WM_CLOSE          0x0010u
#define WM_QUIT           0x0012u
#define WM_KEYDOWN        0x0100u
#define WM_LBUTTONDOWN    0x0201u

#define WS_POPUP          0x80000000u
#define WS_VISIBLE        0x10000000u

#define DDSD_CAPS         0x00000001u
#define DDSCAPS_PRIMARYSURFACE 0x00000010u

#define DDSCL_FULLSCREEN      0x00000001u
#define DDSCL_NOWINDOWCHANGES 0x00001000u

#define DSSCL_PRIORITY     0x00000008u

#define DSBCAPS_GLOBALFOCUS 0x00000002u
#define DSBPLAY_LOOPING     0x00000001u
#define DSBLOCK_ENTIREBUFFER 0x00000002u

#define DD_OK 0x00000000u
#define DS_OK 0x00000000u
#define PM_REMOVE 1u
#define TRANSPARENT_BKMODE 1u
#define TA_LEFT_TOP 0u
#define DEFAULT_GUI_FONT 17

#define SM_W 640
#define SM_H 480
#define SM_BPP 16
#define SM_FRAMES 600                      /* ~10 s at 60 Hz, then it quits */
#define SM_RATE 22050u

/* The synthetic guest's two functions, keyed by the VAs a Win32 program would
 * have put in its class record and its entry point. */
#define VA_WNDPROC 0x00401000u
#define VA_GUEST   0x00402000u

/* ---- host-side state the lifted code updates ---- */
static volatile int g_wndmessages;
static volatile int g_clicks;
static volatile int g_closed;
static volatile uint32_t g_hinst;

/* ---- guest memory ---- */
static uint8_t  g_wc[80];
static char     g_wname[32];
static char     g_wtitle[64];
static char     g_caption[96];
static uint8_t  g_dddesc[128];
static uint8_t  g_dsdesc[40];
static uint8_t  g_wfx[24];
static uint8_t  g_msg[28];
static uint8_t  g_guid[16];
static uint32_t g_ptr[8];
static uint32_t g_sblk[8];

#define VAG(voff)  ((uint32_t)(uintptr_t)((uint8_t *)(voff)))
#define WR32(p, v)  do { MEM32((uint32_t)(uintptr_t)(p)) = (uint32_t)(v); } while (0)
#define WR16(p, v)  do { MEM16((uint32_t)(uintptr_t)(p)) = (uint16_t)(v); } while (0)

/* The shim VAs, resolved once from the shim registry. */
static uint32_t VA_RegisterClassA, VA_CreateWindowExA, VA_ShowWindow, VA_PostQuitMessage;
static uint32_t VA_PeekMessageA, VA_PostMessageA, VA_DestroyWindow, VA_GetModuleHandleA;
static uint32_t VA_GetStockObject, VA_SelectObject, VA_SetTextColor, VA_SetBkMode;
static uint32_t VA_SetTextAlign, VA_TextOutA, VA_ReleaseDC;
static uint32_t VA_DirectDrawCreate, VA_DirectSoundCreate;
static uint32_t VA_dd_SetDisplayMode, VA_dd_SetCooperativeLevel, VA_dd_CreateSurface, VA_dd_Release;
static uint32_t VA_sf_Lock, VA_sf_Unlock, VA_sf_GetDC, VA_sf_Release;
static uint32_t VA_ds_CreateSoundBuffer, VA_ds_SetCooperativeLevel;
static uint32_t VA_sb_Lock, VA_sb_Unlock, VA_sb_Play, VA_sb_Stop, VA_sb_SetVolume, VA_sb_GetStatus;

/* A lifted call: arguments are pushed in reverse so arg0 lands where a shim's
 * A32(0) reads it, then RECOMP_ICALL pushes the return address itself. */
#define GC1(va, a0)  do { PUSH32(esp, (uint32_t)(a0)); RECOMP_ICALL(va); } while (0)
#define GC2(va, a0, a1) do { PUSH32(esp, (uint32_t)(a1)); PUSH32(esp, (uint32_t)(a0)); RECOMP_ICALL(va); } while (0)
#define GC3(va, a0, a1, a2) do { PUSH32(esp, (uint32_t)(a2)); PUSH32(esp, (uint32_t)(a1)); \
                                 PUSH32(esp, (uint32_t)(a0)); RECOMP_ICALL(va); } while (0)
#define GC4(va, a0, a1, a2, a3) do { PUSH32(esp, (uint32_t)(a3)); PUSH32(esp, (uint32_t)(a2)); \
                                       PUSH32(esp, (uint32_t)(a1)); PUSH32(esp, (uint32_t)(a0)); RECOMP_ICALL(va); } while (0)
#define GC5(va, a0, a1, a2, a3, a4) do { PUSH32(esp, (uint32_t)(a4)); PUSH32(esp, (uint32_t)(a3)); \
                                           PUSH32(esp, (uint32_t)(a2)); PUSH32(esp, (uint32_t)(a1)); \
                                           PUSH32(esp, (uint32_t)(a0)); RECOMP_ICALL(va); } while (0)
#define GC8(va, a0, a1, a2, a3, a4, a5, a6, a7) do { PUSH32(esp, (uint32_t)(a7)); PUSH32(esp, (uint32_t)(a6)); \
                                           PUSH32(esp, (uint32_t)(a5)); PUSH32(esp, (uint32_t)(a4)); \
                                           PUSH32(esp, (uint32_t)(a3)); PUSH32(esp, (uint32_t)(a2)); \
                                           PUSH32(esp, (uint32_t)(a1)); PUSH32(esp, (uint32_t)(a0)); RECOMP_ICALL(va); } while (0)
#define GC12(va, a0, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11) do { \
    PUSH32(esp, (uint32_t)(a11)); PUSH32(esp, (uint32_t)(a10)); PUSH32(esp, (uint32_t)(a9)); \
    PUSH32(esp, (uint32_t)(a8));  PUSH32(esp, (uint32_t)(a7));  PUSH32(esp, (uint32_t)(a6)); \
    PUSH32(esp, (uint32_t)(a5));  PUSH32(esp, (uint32_t)(a4));  PUSH32(esp, (uint32_t)(a3)); \
    PUSH32(esp, (uint32_t)(a2));  PUSH32(esp, (uint32_t)(a1));  PUSH32(esp, (uint32_t)(a0)); RECOMP_ICALL(va); } while (0)

#define GUEST_PROLOGUE(va) \
    uint32_t _cf = 0; uint32_t _flag_k = FK_NONE; uint32_t _flag_a = 0, _flag_b = 0; RECOMP_ENTER(va)

/* ---- the window procedure, as a lifted function ----
 * A real game's WndProc is where its frame is drawn and where it decides to
 * quit. Ours only counts, so the test can prove the messages arrived -- which
 * is exactly what screen_ios.c's input queue feeds. */
static void guest_wndproc(void) {
    GUEST_PROLOGUE(VA_WNDPROC);
    uint32_t msg = A32(1);
    g_wndmessages++;
    if (msg == WM_LBUTTONDOWN) g_clicks++;
    if (msg == WM_KEYDOWN)     g_clicks++;
    if (msg == WM_CLOSE)       g_closed = 1;
    eax = 0;
}

/* ---- the test, as a lifted function ---- */
static void guest_main(void) {
    GUEST_PROLOGUE(VA_GUEST);
    uint32_t hinst = g_hinst;
    uint32_t hwnd = 0, dd = 0, ds = 0, prim = 0, sb = 0, font = 0, hdc = 0;
    uint32_t cls, hr, f;

    /* --- the window class --- */
    snprintf(g_wname, sizeof g_wname, "HLEIOSSmoke");
    snprintf(g_wtitle, sizeof g_wtitle, "win32hle on iOS");
    memset(g_wc, 0, sizeof g_wc);
    WR32(g_wc + 0,  0x0003u);                       /* CS_HREDRAW|CS_VREDRAW */
    WR32(g_wc + 4,  VA_WNDPROC);                    /* lpfnWndProc */
    WR32(g_wc + 12, 0);                             /* cbWndExtra */
    WR32(g_wc + 16, hinst);                         /* hInstance */
    WR32(g_wc + 28, 0);                             /* hbrBackground: none */
    WR32(g_wc + 36, VAG(g_wname));
    GC1(VA_RegisterClassA, VAG(g_wc));
    cls = eax;

    /* --- DirectDraw: the mode, then the window, then the primary surface --- */
    memset(g_guid, 0, sizeof g_guid);
    g_ptr[0] = 0;
    GC3(VA_DirectDrawCreate, VAG(g_guid), VAG(&g_ptr[0]), 0);
    dd = g_ptr[0];
    if (!dd) { fprintf(stderr, "[smoke] DirectDrawCreate failed\n"); eax = 1; return; }

    GC4(VA_dd_SetDisplayMode, dd, SM_W, SM_H, SM_BPP);
    if ((hr = eax) != DD_OK) { fprintf(stderr, "[smoke] SetDisplayMode -> 0x%X\n", hr); eax = 1; return; }

    GC12(VA_CreateWindowExA, 0, VAG(g_wname), VAG(g_wtitle), WS_POPUP | WS_VISIBLE,
         0, 0, SM_W, SM_H, 0, 0, hinst, 0);
    hwnd = eax;
    if (!hwnd) { fprintf(stderr, "[smoke] CreateWindowExA failed\n"); eax = 1; return; }
    fprintf(stderr, "[smoke] window %p, class 0x%X\n", (void *)(uintptr_t)hwnd, cls);

    GC3(VA_dd_SetCooperativeLevel, dd, hwnd, DDSCL_FULLSCREEN | DDSCL_NOWINDOWCHANGES);
    if ((hr = eax) != DD_OK) fprintf(stderr, "[smoke] dd SetCooperativeLevel -> 0x%X\n", hr);

    memset(g_dddesc, 0, sizeof g_dddesc);
    WR32(g_dddesc + 0,   108u);                     /* dwSize */
    WR32(g_dddesc + 4,   DDSD_CAPS);
    WR32(g_dddesc + 104, DDSCAPS_PRIMARYSURFACE);
    g_ptr[1] = 0;
    GC4(VA_dd_CreateSurface, dd, VAG(g_dddesc), VAG(&g_ptr[1]), 0);
    prim = g_ptr[1];
    if (!prim) { fprintf(stderr, "[smoke] CreateSurface(primary) -> 0x%X\n", eax); eax = 1; return; }

    /* --- DirectSound: a looping triangle wave in a secondary buffer --- */
    g_ptr[2] = 0;
    GC3(VA_DirectSoundCreate, VAG(g_guid), VAG(&g_ptr[2]), 0);
    ds = g_ptr[2];
    if (ds) {
        GC3(VA_ds_SetCooperativeLevel, ds, hwnd, DSSCL_PRIORITY);
        /* WAVEFORMATEX: tag 1 (PCM), 2 ch, 22050 Hz, 16 bit, block align 4 */
        WR16(g_wfx + 0,  1u);
        WR16(g_wfx + 2,  2u);
        WR32(g_wfx + 4,  SM_RATE);
        WR32(g_wfx + 8,  SM_RATE * 4u);
        WR16(g_wfx + 12, 4u);
        WR16(g_wfx + 14, 16u);
        memset(g_dsdesc, 0, sizeof g_dsdesc);
        WR32(g_dsdesc + 4,  DSBCAPS_GLOBALFOCUS);
        WR32(g_dsdesc + 8,  SM_RATE * 2u);          /* one second */
        WR32(g_dsdesc + 16, VAG(g_wfx));
        g_ptr[3] = 0;
        GC4(VA_ds_CreateSoundBuffer, ds, VAG(g_dsdesc), VAG(&g_ptr[3]), 0);
        sb = g_ptr[3];
        if (!sb) {
            fprintf(stderr, "[smoke] CreateSoundBuffer -> 0x%X (no audio session?)\n", eax);
        } else {
            int16_t *pcm = 0;
            uint32_t n1 = 0;
            g_sblk[0] = g_sblk[1] = 0;
            GC8(VA_sb_Lock, sb, 0, SM_RATE * 4u, VAG(&g_sblk[0]), VAG(&g_sblk[1]), 0, 0, DSBLOCK_ENTIREBUFFER);
            if ((hr = eax) == DS_OK) {
                pcm = (int16_t *)(uintptr_t)g_sblk[0];
                n1 = g_sblk[1];
            }
            if (pcm && n1 >= 8) {
                for (uint32_t i = 0; i * 2 < n1; i++) {
                    uint32_t v = i & 1023u;
                    v = (v < 512u ? v : 1023u - v) - 512u;      /* a triangle */
                    pcm[i] = (int16_t)((int32_t)v * 24);
                }
                fprintf(stderr, "[smoke] audio: %u bytes locked\n", n1);
            }
            GC5(VA_sb_Unlock, sb, g_sblk[0], g_sblk[1], 0, 0);
            GC2(VA_sb_SetVolume, sb, 4000);
            GC4(VA_sb_Play, sb, 0, 0, DSBPLAY_LOOPING);
            if ((hr = eax) == DS_OK) fprintf(stderr, "[smoke] sound playing\n");
        }
    } else {
        fprintf(stderr, "[smoke] DirectSoundCreate -> 0x%X (no audio session)\n", eax);
    }

    /* --- a font for the DC text, rendered by gdidc_ios.c through CoreText --- */
    GC1(VA_GetStockObject, DEFAULT_GUI_FONT);
    font = eax;

    /* --- the frame loop --- */
    fprintf(stderr, "[smoke] %d frames at %dx%d\n", SM_FRAMES, SM_W, SM_H);
    for (f = 0; f < SM_FRAMES; f++) {
        uint16_t *px;
        int pitch, bx, bw = 24;

        /* draw into the primary surface */
        GC5(VA_sf_Lock, prim, 0, VAG(g_dddesc), 0, 0);
        px = (uint16_t *)(uintptr_t)MEM32(VAG(g_dddesc) + 36);
        pitch = (int)MEM32(VAG(g_dddesc) + 16) / 2;
        if (px && pitch >= SM_W) {
            for (int y = 0; y < SM_H; y++) {
                uint16_t *row = px + (size_t)y * (size_t)pitch;
                uint16_t base = (uint16_t)((((uint32_t)((y + f * 2) & 31) * 8u) & 31u) << 11 |
                                           ((((uint32_t)(SM_H - y) * 63u / (uint32_t)SM_H) & 63u) << 5));
                for (int x = 0; x < SM_W; x++) row[x] = base;
            }
            bx = (int)((f * 5u) % (uint32_t)SM_W);
            for (int y = 0; y < SM_H; y++) {
                uint16_t *row = px + (size_t)y * (size_t)pitch;
                uint32_t r5 = (uint32_t)((f + (uint32_t)y) & 31u);
                uint32_t g6 = (f * 3u + (uint32_t)y) & 63u;
                uint32_t b5 = (63u - r5) & 31u;
                uint16_t bar = (uint16_t)((r5 << 11) | (g6 << 5) | b5);
                for (int k = 0; k < bw; k++) {
                    int x = bx + k;
                    if (x < SM_W) row[x] = bar;
                    else row[x - SM_W] = bar;                       /* wrap */
                }
            }
        }
        GC2(VA_sf_Unlock, prim, 0);

        /* text on the surface, through a DC and CoreText */
        snprintf(g_caption, sizeof g_caption,
                 "win32hle on iOS  frame %u/%d  %dx%d %d bpp", f, SM_FRAMES, SM_W, SM_H, SM_BPP);
        g_ptr[4] = 0;
        GC2(VA_sf_GetDC, prim, VAG(&g_ptr[4]));
        hdc = g_ptr[4];
        if (hdc) {
            if (font) GC2(VA_SelectObject, hdc, font);
            GC2(VA_SetTextAlign, hdc, TA_LEFT_TOP);
            GC2(VA_SetBkMode, hdc, TRANSPARENT_BKMODE);
            GC2(VA_SetTextColor, hdc, 0x00FFFFFFu);
            GC5(VA_TextOutA, hdc, 16, 16, VAG(g_caption), (uint32_t)strlen(g_caption));
            GC2(VA_ReleaseDC, hwnd, hdc);
            hdc = 0;
        }

        /* pump: this is where screen_ios.c presents the frame and drains input */
        GC5(VA_PeekMessageA, VAG(g_msg), 0, 0, 0, PM_REMOVE);

        if (!(f % 120)) fprintf(stderr, "[smoke] frame %u, messages %d, clicks %d\n",
                                f, g_wndmessages, g_clicks);
        if (g_closed) { fprintf(stderr, "[smoke] window closed at frame %u\n", f); break; }
    }

    /* --- wind down --- */
    if (sb) GC1(VA_sb_Stop, sb);
    if (prim) GC1(VA_sf_Release, prim);
    if (dd) GC1(VA_dd_Release, dd);
    GC2(VA_ShowWindow, hwnd, 0);
    GC1(VA_DestroyWindow, hwnd);
    GC1(VA_PostQuitMessage, 0);

    /* a real GetMessage loop, so the quit is observed the way a game observes it */
    for (;;) {
        GC5(VA_PeekMessageA, VAG(g_msg), 0, 0, 0, PM_REMOVE);
        if (MEM32(VAG(g_msg) + 4) == WM_QUIT) break;
    }

    fprintf(stderr, "[smoke] done: %d frames, %d window messages, %d clicks, close=%d\n",
            SM_FRAMES, g_wndmessages, g_clicks, g_closed);
    eax = 0;
}

/* The dispatch table the lifted model dispatches through. A real program's is
 * generated from the lift; this one is two hand-written functions. */
const recomp_dispatch_entry_t recomp_dispatch_table[] = {
    { VA_WNDPROC, guest_wndproc },
    { VA_GUEST,   guest_main   },
};
const uint32_t recomp_dispatch_count = 2;

/* ---- host side ---- */
static int resolve(const char *name, uint32_t *slot) {
    uint32_t va = hle_resolve(name);
    if (!va) { fprintf(stderr, "[smoke] missing shim: %s\n", name); return 0; }
    *slot = va;
    return 1;
}

#define RES(field, name) do { if (!resolve((name), &VA_##field)) ok = 0; } while (0)

int ra2_smoke_main(void) {
    int ok = 1;
    uint32_t args[3];

    recomp_host_init();

    RES(RegisterClassA, "RegisterClassA");
    RES(CreateWindowExA, "CreateWindowExA");
    RES(ShowWindow, "ShowWindow");
    RES(DestroyWindow, "DestroyWindow");
    RES(PostQuitMessage, "PostQuitMessage");
    RES(PostMessageA, "PostMessageA");
    RES(PeekMessageA, "PeekMessageA");
    RES(GetModuleHandleA, "GetModuleHandleA");
    RES(GetStockObject, "GetStockObject");
    RES(SelectObject, "SelectObject");
    RES(SetTextColor, "SetTextColor");
    RES(SetBkMode, "SetBkMode");
    RES(SetTextAlign, "SetTextAlign");
    RES(TextOutA, "TextOutA");
    RES(ReleaseDC, "ReleaseDC");
    RES(DirectDrawCreate, "DirectDrawCreate");
    RES(DirectSoundCreate, "DirectSoundCreate");
    if (!ok) return 1;

    args[0] = 0;
    g_hinst = hle_call_guest(VA_GetModuleHandleA, 1, args);
    fprintf(stderr, "[smoke] hInstance %p\n", (void *)(uintptr_t)g_hinst);

    /* DirectDrawCreate / DirectSoundCreate register their COM method shims, so
     * those resolve only after one of them has actually been called. Both are
     * idempotent, and the guest calls them again. */
    memset(g_guid, 0, sizeof g_guid);
    g_ptr[0] = 0;
    args[0] = VAG(g_guid); args[1] = VAG(&g_ptr[0]); args[2] = 0;
    hle_call_guest(VA_DirectDrawCreate, 3, args);
    g_ptr[2] = 0;
    args[1] = VAG(&g_ptr[2]);
    hle_call_guest(VA_DirectSoundCreate, 3, args);

    RES(dd_SetDisplayMode, "IDirectDraw::SetDisplayMode");
    RES(dd_SetCooperativeLevel, "IDirectDraw::SetCooperativeLevel");
    RES(dd_CreateSurface, "IDirectDraw::CreateSurface");
    RES(dd_Release, "IDirectDraw::Release");
    RES(sf_Lock, "IDirectDrawSurface::Lock");
    RES(sf_Unlock, "IDirectDrawSurface::Unlock");
    RES(sf_GetDC, "IDirectDrawSurface::GetDC");
    RES(sf_Release, "IDirectDrawSurface::Release");
    RES(ds_CreateSoundBuffer, "IDirectSound::CreateSoundBuffer");
    RES(ds_SetCooperativeLevel, "IDirectSound::SetCooperativeLevel");
    RES(sb_Lock, "IDirectSoundBuffer::Lock");
    RES(sb_Unlock, "IDirectSoundBuffer::Unlock");
    RES(sb_Play, "IDirectSoundBuffer::Play");
    RES(sb_Stop, "IDirectSoundBuffer::Stop");
    RES(sb_SetVolume, "IDirectSoundBuffer::SetVolume");
    RES(sb_GetStatus, "IDirectSoundBuffer::GetStatus");
    if (!ok) return 1;

    /* The display, then the rest of the machine. hle_screen_open blocks until
     * ios/main.m has an EAGL view to draw into. */
    hle_screen_open("win32hle on iOS", 1, 0);

    /* The guest runs in the lifted model on this thread: it holds the machine
     * for the whole test, which is what a single-threaded Win32 program does. */
    mach_enter();
    guest_main();
    mach_leave();

    fprintf(stderr, "[smoke] guest returned %u\n", g_eax);
    hle_exit(0);
    return 0;
}
/* What ios/main.m's app delegate starts on its worker thread. */
int hle_ios_run_guest(void) { return ra2_smoke_main(); }
