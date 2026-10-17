# win32hle on iOS

win32hle is the Win32 implementation pcrecomp runs recompiled Windows code on
top of: a register file, a simulated TIB, shim tables that answer kernel32 /
user32 / gdi32 / DirectDraw / DirectSound / BINKW32 calls, and a message loop
that pumps the display from inside `GetMessage` / `PeekMessage`. It is plain
C with no inline assembly and no dependency on an OS exception handler, which
is exactly the property that lets it build for a 32-bit ARM phone.

This directory is the iOS port of that runtime. It replaces the four SDL2
files with iOS ones and leaves everything else alone.

## What runs where

The guest runs its own Win32 message loop on a **worker pthread**. UIKit
delivers touches only on the main run loop, so the guest cannot own it; and
OpenGL ES contexts belong to the main thread. The two are connected by two
triple buffers and a queue of input events, nothing more:

```
  main thread                        guest pthread
  -----------                        --------------
  UIApplicationMain
  HLEGLView (EAGL, CADisplayLink)    ra2_smoke_main() / the game's WinMain
    |                                        |
    |  hle_ios_frame_acquire()  <-----------+-- publishes RGBA8888 frames
    |  hle_ios_input_push()     ----------->  a queue the pump drains
    |  hle_ios_view_created()
```

`screen_ios.c` converts whatever the guest's DirectDraw surface holds into
RGBA8888 and publishes it; `HLEGLView.m` uploads it as a GL texture and draws
it letterboxed. `screen_ios.c` also owns the cursor: there is no OS pointer on
a touch screen, so the Win32 AND/XOR mask becomes an RGBA image the presenter
blits on top.

## The files

| file | what it replaces |
| --- | --- |
| `screen_ios.c/.h` | `screen.c`, `present.c` — SDL2 window + renderer + input |
| `dsound_ios.c` | `dsound.c` — SDL2 audio device; the mixer is verbatim |
| `gdidc_ios.c` | `gdidc.c` — SDL2_ttf text; CoreText instead |
| `HLEGLView.m` | the OpenGL ES 2.0 presenter and touch handling |
| `HLEAppDelegate.m`, `HLEViewController.m`, `main.m` | `UIApplicationMain` plumbing |
| `hle_hid.c` | USB HID usage → Win32 VK + scan code (see below) |
| `hle_ios_compat.h/.c` | Darwin naming, `clock_gettime`, `MAP_ANON`, ... |
| `hle_tls.h/.c` | `__thread`, which this clang will not emit for Mach-O |
| `bink_none.c` | an empty `win32hle_bink` table when built without FFmpeg |
| `smoke.c` | the game: a synthetic guest that drives every layer |
| `build.sh`, `Info.plist` | the app build |

`smoke.c` stands in for a lifted game. It has no generated dispatch table
behind it — it defines a two-entry one — and it does what a game does: create
a window, set a display mode, create a primary DirectDraw surface and animate
it, make DirectSound noise, draw text into the surface through a GDI DC, and
loop on `PeekMessage` until quit. Everything below the guest is the real
runtime, so a working smoke app means the port works; when a real lift exists,
`smoke.c` is simply deleted and `recomp_dispatch_table` comes from the
generated code.

## Building

Everything runs on Linux with the prebuilt theos clang. No macOS, no Xcode.

```sh
ARCHS="armv7 armv7s" ./build.sh
```

The default inputs are the ones on this machine:

| what | default |
| --- | --- |
| clang | `/home/jjr/theos/toolchain/linux/iphone/bin/clang` |
| SDK | `/home/jjr/theos/sdks/iPhoneOS6.0.sdk` (arm64 uses 10.3) |
| FFmpeg | `../../../ffmpeg-ios/dist` |

| variable | meaning |
| --- | --- |
| `ARCHS` | `armv7 armv7s` by default; `arm64` is available too |
| `WITH_BINK` | 1 links FFmpeg and answers BINKW32; 0 leaves movies out |
| `DO_PACKAGE` | 0 stops after linking |
| `APP_NAME` | bundle name, default `win32hle` |
| `FFMPEG` | directory with `libav*.a` and `include/` |

Output is a fat Mach-O and `win32hle.ipa` holding `Payload/win32hle.app`,
signed with `ldid`.

### FFmpeg

Only BINKW32 needs FFmpeg, and only for the `bink` demuxer plus the `bink` and
`binkaudio` decoders — everything else is dead weight. Build it with the
`bink` profile of the sibling `ffmpeg-ios` repo:

```sh
gh release download ffmpeg-7.1-bink-ios --repo JiJingren/ffmpeg-ios
```

The full profile does not link on armv7 at all: FFmpeg's EXR and PNM encoders
call `__extendhfsf2` and `__truncsfhf2`, the 128-bit half-float helpers from
compiler-rt, and the theos toolchain ships no compiler-rt. The `bink` profile
builds neither codec.

## On the device

**Gestures**

| gesture | what the game sees |
| --- | --- |
| one finger | left button down + mouse move |
| tap | left click (press and release where you lifted) |
| two taps in a row, close together | double click |
| long press | right click |
| second finger | right click |
| three-finger tap | cycle scaling mode, show it on screen |

Scaling cycles `sharp → smooth → crt → nearest → integer`, the same five modes
`screen.c` offers.

**Keyboard.** iOS 6 has no hardware keyboard API, and the theos SDK is a
partial copy with no `UIPress` at all, so no key events are delivered. The
seam is `hle_hid_to_win()` in `hle_hid.c`, which maps USB HID Keyboard/Keypad
usages to Win32 virtual keys and PC/AT scan codes, and
`-[HLEGLView pressHidUsage:down:]`, which pushes them. A build against a full
Xcode SDK only needs to call the second from `pressesBegan:` / `pressesEnded:`.

**Leaving the app.** `applicationDidEnterBackground:` asks the guest to close
its window and exit. Nothing in a Win32 game survives a suspend — the DirectDraw
surfaces and the sound buffer are gone — so this behaves like closing the
window on the desktop.

## Porting notes

Three things in the upstream runtime do not build for Mach-O with the clang
this project uses. Each is handled without touching the C semantics.

**Thread-local storage.** clang 11 rejects `__thread` for every Mach-O target
("thread-local storage is not supported for the current target") and its
`-femulated-tls` does not help. `hle_tls.h` declares each of the eight
thread-local variables behind `HLE_TLS_LOCAL` / `HLE_TLS_ARRAY` and, with
`-DHLE_PTHREAD_TLS`, turns the name into a `pthread_getspecific` lookup. On
every other target it expands to `static __thread` and nothing changes. The
variables are the per-thread register file, the blocking generation counter,
the last-error and thread-id values, the 64-slot TLS block, and three Winsock
scratch buffers.

**`clock_gettime`.** It is iOS 10 and later; the iOS 6 SDK does not declare it.
`hle_ios_compat.c` provides it on top of `gettimeofday` and
`mach_absolute_time`.

**Spelling.** `MAP_ANONYMOUS` is `MAP_ANON`, `MSG_NOSIGNAL` does not exist,
`stat` times are `st_mtimespec` / `st_mtimespec`, and `SetFileTime` has to use
`futimes()` because Darwin has neither `futimens` nor `UTIME_OMIT`.

Nothing here needs x87 extended precision: pcrecomp models the FPU stack as
`double` and reads 80-bit memory operands through explicit conversions, which
is why a Win32 x86 game recompiles to 32-bit ARM at all.