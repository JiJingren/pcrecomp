/*
 * hle_ios_compat.h - the POSIX spellings win32hle uses that Darwin's headers
 * do not have, for the iOS build.
 *
 * Force-included (cc -include hle_ios_compat.h) by ios/build.sh, so no shim
 * source needs a line added for it. Everything here is a gap between glibc and
 * Darwin, not a change in behaviour: Darwin has MAP_ANON under another name,
 * has no SIGPIPE-on-send flag because it has no signal from send(), and gained
 * clock_gettime() in iOS 10 - while the iOS 6 SDK this port targets is older
 * than that, so it declares neither the call nor its CLOCK_* ids.
 *
 * Only applied when the target is Darwin and CLOCK_MONOTONIC is missing, so a
 * newer SDK, or a host build, keeps using the real thing.
 */
#ifndef HLE_IOS_COMPAT_H
#define HLE_IOS_COMPAT_H

#if defined(__APPLE__)

#include <stddef.h>
#include <time.h>

/* MAP_ANON is Darwin's name for what glibc calls MAP_ANONYMOUS. pe_loader.c
 * asks for MAP_FIXED_NOREPLACE too; Darwin has no such flag, so it maps
 * anywhere and then checks (see hle_map_fixed_noreplace's comment) - but the
 * iOS app does not load a PE at all, so the define is here only so the file
 * still compiles if it is built in. */
#ifndef MAP_ANONYMOUS
#define MAP_ANONYMOUS MAP_ANON
#endif
#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0
#endif

/* No SIGPIPE from send(): Darwin has MSG_NOSIGNAL's behaviour already. */
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

/* Darwin calls the two timestamps st_mtimespec / st_atimespec; glibc calls them
 * st_mtim / st_atim. kernel32_ext.c's FILETIME-from-stat needs the latter. */
#ifndef __GLIBC__
#define st_mtim st_mtimespec
#define st_atim st_atimespec
#endif

#if !defined(CLOCK_MONOTONIC)

#define CLOCK_REALTIME  0
#define CLOCK_MONOTONIC 1

/* mach_absolute_time() is what the monotonic clock is built from on Darwin, and
 * it exists since forever, so it is also what an iOS 6 deployment target gets.
 * For CLOCK_REALTIME the wall clock is needed, which is mach_absolute_time's
 * epoch plus the boot-to-wall offset the kernel already tracks. */
#define HLE_IOS_COMPAT_CLOCK 1
int hle_clock_gettime(int clock_id, struct timespec *ts);
#define clock_gettime hle_clock_gettime

#endif /* !CLOCK_MONOTONIC */

#endif /* __APPLE__ */

#endif /* HLE_IOS_COMPAT_H */