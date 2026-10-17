/*
 * bink_none.c - the BINKW32 table when the app is built without FFmpeg.
 *
 * bink.c is entirely inside #ifdef HLE_WITH_FFMPEG, so without it there is no
 * win32hle_bink symbol at all and host.c fails to link. An empty table is the
 * honest equivalent: BinkOpen does not resolve, the game treats its movie as
 * missing, which is what bink.c's own non-FFMPEG path does.
 */
#include <stdint.h>
#include "win32hle.h"

const win32hle_shim win32hle_bink[] = { { 0, 0 } };