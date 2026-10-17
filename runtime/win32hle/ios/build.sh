#!/usr/bin/env bash
#
# ios/build.sh - build the win32hle iOS smoke app.
#
# Everything here runs on Linux with the prebuilt theos clang; no macOS, no
# Xcode. The SDK has to be a real device SDK - armv7 and armv7s only exist on
# the device, and there is no i386/x86_64 slice to bother with because this
# port never runs in the simulator.
#
# Env:
#   ARCHS       space separated archs (default "armv7 armv7s")
#   IOS_CLANG   clang to use (default $TOOLCHAIN/linux/iphone/bin/clang)
#   SDKROOT     dir holding iPhoneOS6.0.sdk / iPhoneOS10.3.sdk
#               (default /home/jjr/theos/sdks)
#   SDK_<arch>  full path to the SDK for one arch, overriding SDKROOT. Needed
#               on CI, where theos/sdks has no iPhoneOS6.0.sdk: iPhoneOS9.3
#               carries armv7 and armv7s and builds the same code fine, as
#               long as the deployment target stays at 6.0.
#   FFMPEG      dir with libav*.a + include (default ../../../ffmpeg-ios/dist)
#   OUT         build tree (default ./.build)
#   APP_NAME    bundle name (default win32hle)
#   DO_PACKAGE  1 = build the .ipa (default), 0 = stop after linking
#   WITH_BINK   1 = link FFmpeg and answer BINKW32 (default), 0 = leave the
#               Bink shims out, so BinkOpen fails and movies are skipped
#
# Produces, per arch, $OUT/<arch>/win32hle, then a fat binary and (with
# DO_PACKAGE=1) $OUT/win32hle.ipa holding Payload/win32hle.app.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
HLE="$(dirname "$HERE")"
ROOT="$(dirname "$(dirname "$HLE")")"          # runtime/.. == pcrecomp root

ARCHS="${ARCHS:-armv7 armv7s}"
SDKROOT="${SDKROOT:-/home/jjr/theos/sdks}"
TOOLCHAIN="${TOOLCHAIN:-/home/jjr/theos/toolchain}"
CC_BIN="${IOS_CLANG:-$TOOLCHAIN/linux/iphone/bin/clang}"
FFMPEG="${FFMPEG:-$ROOT/../ffmpeg-ios/dist}"
OUT="${OUT:-$HERE/.build}"
APP_NAME="${APP_NAME:-win32hle}"
DO_PACKAGE="${DO_PACKAGE:-1}"
WITH_BINK="${WITH_BINK:-1}"

BUNDLE_ID="org.pcrecomp.$APP_NAME"

[ -x "$CC_BIN" ] || { echo "no clang at $CC_BIN" >&2; exit 1; }
if [ "$WITH_BINK" = 1 ]; then
  [ -d "$FFMPEG" ] || { echo "no ffmpeg dist at $FFMPEG (set FFMPEG, or WITH_BINK=0)" >&2; exit 1; }
fi

# ---------------------------------------------------------------- arch map
# armv7 and armv7s share the 2012 SDK; arm64 needs the 10.3 one because iOS 6
# never ran on 64-bit and the 6.0 SDK has no arm64 slice at all.
declare -A TARGET MINVER SDK
TARGET[armv7]="armv7-apple-ios6.0";  MINVER[armv7]="6.0"; SDK[armv7]="iPhoneOS6.0.sdk"
TARGET[armv7s]="armv7s-apple-ios6.0"; MINVER[armv7s]="6.0"; SDK[armv7s]="iPhoneOS6.0.sdk"
TARGET[arm64]="arm64-apple-ios7.0";  MINVER[arm64]="7.0"; SDK[arm64]="iPhoneOS10.3.sdk"

for a in "${!TARGET[@]}"; do
  eval "SDK[$a]=\${SDK_$a:-\$SDKROOT/\${SDK[$a]}}"
done

for a in $ARCHS; do
  [ -n "${TARGET[$a]:-}" ] || { echo "unsupported arch $a" >&2; exit 1; }
  [ -d "${SDK[$a]}" ] || { echo "missing SDK for $a: ${SDK[$a]}" >&2; exit 1; }
done

# ---------------------------------------------------------------- sources
# Every shim module except the four that are SDL2 (screen/present/dsound/gdidc)
# and the selftests; the iOS replacements and the smoke guest come from ios/.
SHIM_C=(
  advapi32.c ddraw.c gdi32.c hle_path.c hle_tls.c host.c host_lite.c
  kernel32.c kernel32_crt.c kernel32_ext.c module.c ole32.c pe_loader.c
  storage.c user32.c winmm.c wsock32.c
)
IOS_C=(
  ios/dsound_ios.c ios/gdidc_ios.c ios/hle_ios_compat.c ios/screen_ios.c
  ios/hle_hid.c
)
if [ "$WITH_BINK" = 1 ]; then
  SHIM_C+=(bink.c)
else
  IOS_C+=(ios/bink_none.c)     # keeps host.c's win32hle_bink reference linked
fi
IOS_M=(
  ios/main.m ios/HLEAppDelegate.m ios/HLEViewController.m ios/HLEGLView.m
)
# smoke.c stands in for the lifted game. It is generated-code-shaped, so it
# must not be compiled the way the shims are - see the -DRECOMP_LOCAL_REGS note.
GUEST_C=(ios/smoke.c)

INCS=(-I"$HLE" -I"$ROOT/runtime/recomp32" -I"$FFMPEG/include")

# Darwin spelled a few things differently and the 6.0 SDK predates two of the
# calls win32hle uses; hle_ios_compat.h is force-included into every object.
COMMON_DEFS=(
  -DHLE_PTHREAD_TLS
  -DRECOMP_RETADDR=0u
  -DRECOMP_FLAT_MEMORY
)
[ "$WITH_BINK" = 1 ] && COMMON_DEFS+=(-DHLE_WITH_FFMPEG)
SHIM_DEFS=(-DRECOMP_LOCAL_REGS)

# the shim modules keep a few shims around that the table may not reference
CFLAGS=(-O2 -std=gnu99 -Wall -Wextra -Wno-unused-parameter -Wno-unused-function
        -fno-strict-aliasing)
OBJCFLAGS=(-O2 -fobjc-arc -Wall -Wextra -Wno-unused-parameter)

FRAMEWORKS=(
  -framework Foundation
  -framework UIKit
  -framework QuartzCore
  -framework OpenGLES
  -framework CoreGraphics
  -framework AudioToolbox
  -framework CoreText
)
# AudioUnit.framework in the iOS 6 SDK is a headers-only stub; every AudioUnit
# and AudioComponent symbol lives in AudioToolbox. No -framework AudioUnit.

FFMPEG_LIBS=(
  "$FFMPEG/libswscale.a" "$FFMPEG/libswresample.a" "$FFMPEG/libavfilter.a"
  "$FFMPEG/libavformat.a" "$FFMPEG/libavcodec.a" "$FFMPEG/libavutil.a"
)

# ---------------------------------------------------------------- build
OBJCOMMON=(-include "$HERE/hle_ios_compat.h")

rm -rf "$OUT"
mkdir -p "$OUT"

for arch in $ARCHS; do
  sdk="${SDK[$arch]}"
  target="${TARGET[$arch]}"
  minv="${MINVER[$arch]}"
  objdir="$OUT/$arch"
  mkdir -p "$objdir"

  common=(-target "$target" -isysroot "$sdk" -miphoneos-version-min="$minv")
  # theos' clang warns on every frame; keep the output about our own code.
  # unguarded-availability matters when the SDK is newer than the deployment
  # target, which is how the CI builds armv7: iPhoneOS9.3 headers, min 6.0.
  common+=(-Wno-deprecated-declarations -Wno-error=unguarded-availability)

  echo "=== $arch  $target  (SDK $sdk)"

  srcs=()
  for f in "${SHIM_C[@]}" "${IOS_C[@]}"; do
    srcs+=("$HLE/$f")
  done

  for s in "${srcs[@]}"; do
    o="$objdir/$(basename "$s" .c).o"
    "$CC_BIN" "${common[@]}" "${CFLAGS[@]}" "${COMMON_DEFS[@]}" "${SHIM_DEFS[@]}" \
      "${INCS[@]}" "${OBJCOMMON[@]}" -c "$s" -o "$o"
  done

  for s in "${GUEST_C[@]}"; do
    o="$objdir/$(basename "$s" .c).o"
    "$CC_BIN" "${common[@]}" "${CFLAGS[@]}" "${COMMON_DEFS[@]}" \
      "${INCS[@]}" "${OBJCOMMON[@]}" -c "$HLE/$s" -o "$o"
  done

  # UIKit declared -supportedInterfaceOrientations as returning NSUInteger in
  # the 2012 SDK and as UIInterfaceOrientationMask from 10.3 on. Same bits, so
  # implement the modern spelling and quiet only that warning for this one file.
  for s in "${IOS_M[@]}"; do
    o="$objdir/$(basename "$s" .m).o"
    extra=()
    [ "$(basename "$s")" = HLEViewController.m ] && extra=(-Wno-mismatched-return-types)
    "$CC_BIN" "${common[@]}" "${OBJCFLAGS[@]}" \
      -I"$HERE" "${INCS[@]}" "${OBJCOMMON[@]}" "${extra[@]}" \
      -c "$HERE/$(basename "$s")" -o "$o"
  done

  link_ffmpeg=()
  if [ "$WITH_BINK" = 1 ]; then
    link_ffmpeg=("${FFMPEG_LIBS[@]}" -lz -lbz2 -liconv)
  fi
  "$CC_BIN" "${common[@]}" -o "$objdir/$APP_NAME" "$objdir"/*.o \
    "${link_ffmpeg[@]}" -lm \
    "${FRAMEWORKS[@]}" \
    -Wl,-dead_strip
  echo "    linked $objdir/$APP_NAME ($(stat -c %s "$objdir/$APP_NAME") bytes)"
done

# ---------------------------------------------------------------- fat + ipa
LIPO="$(dirname "$CC_BIN")/lipo"
[ -x "$LIPO" ] || LIPO=lipo

first="$(echo $ARCHS | cut -d' ' -f1)"
if [ "$(echo $ARCHS | wc -w)" -gt 1 ] && "$LIPO" -info "$OUT/$first/$APP_NAME" >/dev/null 2>&1; then
  "$LIPO" -create $(for a in $ARCHS; do printf '%s ' "$OUT/$a/$APP_NAME"; done) \
          -output "$OUT/$APP_NAME"
  echo "=== fat binary: $OUT/$APP_NAME"
  "$LIPO" -info "$OUT/$APP_NAME"
else
  cp "$OUT/$first/$APP_NAME" "$OUT/$APP_NAME"
fi

[ "$DO_PACKAGE" = 1 ] || exit 0

app="$OUT/$APP_NAME.app"
rm -rf "$OUT/Payload" "$app"
mkdir -p "$app"
cp "$OUT/$APP_NAME" "$app/$APP_NAME"
# the two keys that have to agree with what we just named the bundle
perl -0777 -pe 's{(<key>CFBundleExecutable</key>\s*<string>)[^<]*(</string>)}{${1}'"$APP_NAME"'${2}}; s{(<key>CFBundleIdentifier</key>\s*<string>)[^<]*(</string>)}{${1}'"$BUNDLE_ID"'${2}}' \
  "$HERE/Info.plist" > "$app/Info.plist"
grep -q "<string>$APP_NAME</string>" "$app/Info.plist" || {
  echo "!! Info.plist rewrite failed" >&2; exit 1; }

LDID="${LDID:-ldid}"
if command -v "$LDID" >/dev/null 2>&1; then
  "$LDID" -S "$app/$APP_NAME"
  echo "=== signed with ldid"
else
  echo "!! ldid not found, the ipa will need signing on a Mac" >&2
fi

mkdir -p "$OUT/Payload"
mv "$app" "$OUT/Payload/"
( cd "$OUT" && zip -qr9 "$APP_NAME.ipa" Payload )
echo "=== $OUT/$APP_NAME.ipa  ($(stat -c %s "$OUT/$APP_NAME.ipa") bytes)"