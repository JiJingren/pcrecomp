/*
 * hle_tls.h - the __thread variables of the shim layer, for a toolchain with
 * no thread-local storage.
 *
 * The layer uses __thread in eight places (host_lite.c's machine state and
 * block generation, kernel32.c's last error, thread id and TLS array,
 * wsock32.c's Winsock error and its two scratch buffers). A desktop compiler
 * has it for free. The clang build the iOS port cross-compiles with (clang 11
 * built for a Linux host) rejects __thread for every Mach-O target and has no
 * -femulated-tls: its Darwin backend is registered without the frontend's
 * thread-local support, although llc in the same tree does list the arm targets,
 * so the codegen half is there and only the C frontend says no. Nothing on the
 * machine can be told otherwise.
 *
 * So with -DHLE_PTHREAD_TLS each __thread variable becomes one block of memory
 * per host thread, reached through hle_tls_slot(). The block is zeroed on first
 * use - every one of the eight is a zero-initialised type, a register save
 * area, a counter, a last-error code, a 64-slot array - which is what the
 * compiler gave too - and is freed when the thread ends, again as it would
 * have been. A slot is a file-scope __COUNTER__ value, so the sites stay
 * independent and adding one is one line.
 *
 * A declaration becomes:
 *
 *     HLE_TLS_LOCAL(uint32_t, g_last_error);
 *     #if defined(HLE_PTHREAD_TLS)
 *     #define g_last_error (*(uint32_t *)hle_tls_slot(g_last_error_slot, sizeof g_last_error_anchor_))
 *     #endif
 *
 * and an array the same with HLE_TLS_ARRAY, cast to a pointer-to-array so that
 * subscripting and sizeof keep working on the name.
 */
#ifndef HLE_TLS_H
#define HLE_TLS_H

#include <stddef.h>

#if defined(HLE_PTHREAD_TLS)

/* A block for the calling thread, zeroed, of at least size bytes. NULL only on
 * allocation failure. */
void *hle_tls_slot(unsigned slot, size_t size);

#define HLE_TLS_LOCAL(type, name)                                             \
    static const unsigned name##_slot = __COUNTER__;                          \
    static type name##_anchor_ __attribute__((unused))

#define HLE_TLS_ARRAY(type, name, n)                                          \
    static const unsigned name##_slot = __COUNTER__;                          \
    static type name##_anchor_[n] __attribute__((unused))

#else

#define HLE_TLS_LOCAL(type, name) static __thread type name
#define HLE_TLS_ARRAY(type, name, n) static __thread type name[n]

#endif /* HLE_PTHREAD_TLS */

#endif /* HLE_TLS_H */