/*
 * hle_tls.c - the per-thread blocks HLE_PTHREAD_TLS hands out (hle_tls.h).
 * One pthread key holds a small per-thread table; each slot is looked up once
 * and then reused, and the table (with its blocks) is freed by the key's
 * destructor when the thread ends.
 */
#define _GNU_SOURCE
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include "hle_tls.h"

#if defined(HLE_PTHREAD_TLS)

/* Eight sites today; a few spare, and a site past the end is a NULL rather
 * than a corruption, so a new one can be added without a matching bump. */
#define HLE_TLS_SLOTS 16

struct tls_table {
    void *block[HLE_TLS_SLOTS];
    size_t size[HLE_TLS_SLOTS];
};

static pthread_key_t g_key;
static pthread_once_t g_once = PTHREAD_ONCE_INIT;

static void table_free(void *p) {
    struct tls_table *t = (struct tls_table *)p;
    if (!t) return;
    for (int i = 0; i < HLE_TLS_SLOTS; i++) free(t->block[i]);
    free(t);
}

static void key_make(void) { pthread_key_create(&g_key, table_free); }

void *hle_tls_slot(unsigned slot, size_t size) {
    if (slot >= HLE_TLS_SLOTS) return NULL;
    pthread_once(&g_once, key_make);
    struct tls_table *t = (struct tls_table *)pthread_getspecific(g_key);
    if (!t) {
        t = (struct tls_table *)calloc(1, sizeof *t);
        if (!t) return NULL;
        pthread_setspecific(g_key, t);
    }
    if (!t->block[slot]) {
        t->block[slot] = calloc(1, size ? size : 1);
        if (!t->block[slot]) return NULL;
        t->size[slot] = size;
    }
    return t->block[slot];
}

#endif /* HLE_PTHREAD_TLS */