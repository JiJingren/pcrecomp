/*
 * screen_ios.c - screen.c for iOS: the display and input for a DirectDraw game,
 * without SDL2, with the GLES drawing and the touch handling on the main thread
 * (see screen_ios.h for why the split is where it is).
 *
 * What is left here is everything that can be left: the primary surface is
 * converted to RGBA8888 and published, touches and presses are turned into the
 * window messages user32.c already understands, and the scaling mode, the bars
 * setting, the title and the cursor bookkeeping behave as they do on the
 * desktop. Nothing here touches OpenGL or UIKit.
 *
 * The throttling is screen.c's: show a frame when the primary has changed, at
 * most every 15 ms, and show the same one again only after 250 ms.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <time.h>

#include "win32hle.h"
#include "screen_ios.h"

#define WM_MOUSEMOVE   0x0200u
#define WM_LBUTTONDOWN 0x0201u
#define WM_LBUTTONUP   0x0202u
#define WM_RBUTTONDOWN 0x0204u
#define WM_RBUTTONUP   0x0205u
#define WM_CLOSE       0x0010u

/* ---- the hooks a host installs (win32hle.h) ---- */
void (*hle_screen_settings_hook)(const char *scale, int bars, int fullscreen);
int  (*hle_screen_cursor_hook)(void);
int  (*hle_screen_key_hook)(int sdl_scancode, int down);
void (*hle_screen_frame_hook)(const uint8_t *px, int w, int h, int pitch, int bpp);
int  (*hle_screen_compose_hook)(const uint8_t *px16, int pitch, int w, int h, uint32_t *out);

/* ---- the picture, as screen.c offers it ---- */
static const char *const k_modes[] = { "sharp", "smooth", "crt", "nearest", "integer" };
static int g_mode, g_bars = 1, g_fullscreen, g_headless;
static char g_title[256];
static void settings_changed(void);

int hle_screen_scale(const char *name) {
    for (int i = 0; i < 5; i++) if (name && !strcmp(name, k_modes[i])) { g_mode = i; return 1; }
    return 0;
}
void hle_screen_bars(int blur) { g_bars = blur != 0; }

/* F12 is a key the desktop presenter cycles with; a touch screen has none of
 * that, so the settings screen calls these instead. */
int  hle_ios_scale_cycle(void)  { g_mode = (g_mode + 1) % 5; settings_changed(); return g_mode; }
int  hle_ios_scale_set(int mode) { if (mode < 0 || mode > 4) return g_mode; g_mode = mode; settings_changed(); return g_mode; }
int  hle_ios_bars_set(int blur)  { int b = blur != 0; if (b != g_bars) { g_bars = b; settings_changed(); } return g_bars; }
const char *hle_ios_scale_name(void) { return k_modes[g_mode]; }
int  hle_ios_scale_mode(void) { return g_mode; }
int  hle_ios_crt_on(void) { return g_mode == 2; }

void hle_screen_title(const char *title) {
    if (!title) return;
    snprintf(g_title, sizeof g_title, "%s", title);
}
const char *hle_ios_title(void) { return g_title; }

/* ---- the published frame ----
 * Three of them, so the guest can be filling one while the main thread uploads
 * another and the one before that is still on screen. */
#define FB_COUNT 3
static uint32_t *g_fb_px[FB_COUNT];
static int g_fb_w[FB_COUNT], g_fb_h[FB_COUNT];
static int g_fb_write, g_fb_read = -1;
static unsigned g_fb_seq;
static pthread_mutex_t g_fb_lock = PTHREAD_MUTEX_INITIALIZER;

/* The view, in pixels, and the picture's place in it. Touch mapping and drawing
 * both need these and must agree on them, so they live here. */
static int g_view_w, g_view_h, g_dst_x, g_dst_y, g_dst_w, g_dst_h;
static unsigned g_shown_seq = ~0u;
static uint32_t g_last_present;

static void settings_changed(void) {
    g_shown_seq = ~0u;                                  /* show at once, in the new shape */
    if (hle_screen_settings_hook) hle_screen_settings_hook(k_modes[g_mode], g_bars, g_fullscreen);
}

static void layout(void) {
    int w, h, bpp;
    hle_dd_mode(&w, &h, &bpp);
    if (w <= 0 || h <= 0 || g_view_w <= 0 || g_view_h <= 0) {
        g_dst_x = g_dst_y = g_dst_w = g_dst_h = 0;
        return;
    }
    if ((long long)g_view_w * h > (long long)g_view_h * w) {
        g_dst_h = g_view_h, g_dst_w = (int)((long long)g_view_h * w / h);
    } else {
        g_dst_w = g_view_w, g_dst_h = (int)((long long)g_view_w * h / w);
    }
    if (g_mode == 4) {                                   /* integer: whole multiples only */
        int k = g_view_w / w < g_view_h / h ? g_view_w / w : g_view_h / h;
        if (k < 1) k = 1;
        g_dst_w = w * k, g_dst_h = h * k;
    }
    g_dst_x = (g_view_w - g_dst_w) / 2, g_dst_y = (g_view_h - g_dst_h) / 2;
}

void hle_ios_view_size(int w, int h) {
    pthread_mutex_lock(&g_fb_lock);
    if (w != g_view_w || h != g_view_h) {
        g_view_w = w, g_view_h = h;
        layout();
        g_shown_seq = ~0u;                              /* show the new size at once */
    }
    pthread_mutex_unlock(&g_fb_lock);
}
void hle_ios_dst_rect(int *x, int *y, int *w, int *h) {
    pthread_mutex_lock(&g_fb_lock);
    if (x) *x = g_dst_x;
    if (y) *y = g_dst_y;
    if (w) *w = g_dst_w;
    if (h) *h = g_dst_h;
    pthread_mutex_unlock(&g_fb_lock);
}
const uint32_t *hle_ios_frame_acquire(int *w, int *h) {
    const uint32_t *px = NULL;
    pthread_mutex_lock(&g_fb_lock);
    if (g_fb_read >= 0) {
        if (w) *w = g_fb_w[g_fb_read];
        if (h) *h = g_fb_h[g_fb_read];
        px = g_fb_px[g_fb_read];
    }
    pthread_mutex_unlock(&g_fb_lock);
    return px;
}
unsigned hle_ios_frame_seq(void) {
    unsigned s;
    pthread_mutex_lock(&g_fb_lock);
    s = g_fb_seq;
    pthread_mutex_unlock(&g_fb_lock);
    return s;
}

/* ---- the input queue: main thread in, guest thread out ---- */
#define Q_MAX 512
struct input { int kind, x, y, vk, scan; };
static struct input g_q[Q_MAX];
static int g_q_head, g_q_tail;
static pthread_mutex_t g_q_lock = PTHREAD_MUTEX_INITIALIZER;

void hle_ios_input_push(int kind, int x, int y, int vk, int scan) {
    pthread_mutex_lock(&g_q_lock);
    int n = (g_q_head + 1) % Q_MAX;
    if (n != g_q_tail) {                                /* full: drop the oldest */
        g_q[g_q_head].kind = kind, g_q[g_q_head].x = x, g_q[g_q_head].y = y;
        g_q[g_q_head].vk = vk, g_q[g_q_head].scan = scan;
        g_q_head = n;
    }
    pthread_mutex_unlock(&g_q_lock);
}

static int q_pop(struct input *e) {
    int got = 0;
    pthread_mutex_lock(&g_q_lock);
    if (g_q_tail != g_q_head) {
        *e = g_q[g_q_tail];
        g_q_tail = (g_q_tail + 1) % Q_MAX;
        got = 1;
    }
    pthread_mutex_unlock(&g_q_lock);
    return got;
}

/* View pixels -> game pixels, clamped to the picture. hle_ios_input_push takes
 * pixels, so the main thread has already multiplied out the screen scale. */
static void to_game(int x, int y, int *gx, int *gy) {
    int w, h, bpp, dx, dy, dw, dh;
    hle_dd_mode(&w, &h, &bpp);
    pthread_mutex_lock(&g_fb_lock);
    dx = g_dst_x, dy = g_dst_y, dw = g_dst_w, dh = g_dst_h;
    pthread_mutex_unlock(&g_fb_lock);
    if (dw <= 0 || dh <= 0 || w <= 0 || h <= 0) { *gx = x, *gy = y; return; }
    *gx = (int)((long long)(x - dx) * w / dw);
    *gy = (int)((long long)(y - dy) * h / dh);
    if (*gx < 0) *gx = 0;
    if (*gy < 0) *gy = 0;
    if (*gx >= w) *gx = w - 1;
    if (*gy >= h) *gy = h - 1;
}

/* The guest moved the cursor (SetCursorPos). There is no OS cursor on a touch
 * screen, so the guest's new position is posted back as a mouse move: that is
 * what keeps a warp-and-follow cursor working when the finger that caused it is
 * still down. */
static void warp(int x, int y) {
    hle_ios_input_push(HLE_IOS_MOUSE_MOVE, x, y, 0, 0);
}

/* ---- the cursor, from the guest's AND/XOR masks ----
 * A Win32 cursor is 32x32, one bit per pixel, four bytes to a row, and the pair
 * of masks means: AND 1 is transparent, otherwise the colour is the XOR bit
 * (0 black, 1 white). Output is the RGBA8888 the rest of this file uses, with
 * alpha last so GL_RGBA can upload it. */
static uint32_t g_cur_px[32 * 32];
static int g_cur_w, g_cur_hot_x, g_cur_hot_y;
static int g_cur_shown = -1;
static uint32_t g_cur_handle = ~0u;

static void cursor_build(const hle_cursor_image_t *img) {
    for (int y = 0; y < 32; y++) {
        for (int x = 0; x < 32; x++) {
            int and_bit = (img->and_mask[y * 4 + x / 8] >> (7 - x % 8)) & 1;
            int xor_bit = (img->xor_mask[y * 4 + x / 8] >> (7 - x % 8)) & 1;
            uint32_t v = xor_bit ? 0x000000u : 0xFFFFFFu;
            g_cur_px[y * 32 + x] = (v << 24) | (v >> 8 & 0xFF00u) | (v >> 16 & 0xFFu)
                                 | (and_bit ? 0xFF000000u : 0u);
        }
    }
    g_cur_w = 32;
    g_cur_hot_x = img->hot_x, g_cur_hot_y = img->hot_y;
}

static void cursor_update(void) {
    int show = hle_screen_cursor_hook && hle_screen_cursor_hook();
    uint32_t handle = show ? hle_current_cursor() : 0;
    if (show == g_cur_shown && (!show || handle == g_cur_handle)) return;
    g_cur_shown = show;
    if (!show) { g_cur_w = 0; return; }
    g_cur_handle = handle;
    const hle_cursor_image_t *img = hle_cursor_image(handle);
    if (img) cursor_build(img);
    else g_cur_w = 0;
    fprintf(stderr, "[screen] cursor: %s\n",
            !show ? "drawn by the game" : img ? "the game's, from its resources" : "the system arrow");
}

const uint32_t *hle_ios_cursor_acquire(int *w, int *h, int *hot_x, int *hot_y) {
    if (g_cur_w <= 0) return NULL;
    if (w) *w = g_cur_w;
    if (h) *h = g_cur_w;
    if (hot_x) *hot_x = g_cur_hot_x;
    if (hot_y) *hot_y = g_cur_hot_y;
    return g_cur_px;
}

/* Game pixels -> view pixels, the inverse of to_game(). Done here rather than
 * in the drawer so the two stay exact opposites. */
int hle_ios_cursor_screen_pos(int *x, int *y) {
    int gx, gy, w, h, bpp, dx, dy, dw, dh;
    hle_cursor_image_pos(&gx, &gy);
    hle_dd_mode(&w, &h, &bpp);
    pthread_mutex_lock(&g_fb_lock);
    dx = g_dst_x, dy = g_dst_y, dw = g_dst_w, dh = g_dst_h;
    pthread_mutex_unlock(&g_fb_lock);
    if (dw <= 0 || dh <= 0 || w <= 0 || h <= 0) { if (x) *x = gx; if (y) *y = gy; return 0; }
    if (x) *x = dx + (int)((long long)gx * dw / w);
    if (y) *y = dy + (int)((long long)gy * dh / h);
    return 1;
}

/* ---- the frame ---- */
static uint32_t *g_hd;                                   /* the HD layer, the compose hook's */
static size_t g_hd_n;

static uint32_t *fb_px(int w, int h) {
    int i = g_fb_write;
    if (!g_fb_px[i]) g_fb_px[i] = (uint32_t *)calloc((size_t)w * h, sizeof(uint32_t));
    else if (g_fb_w[i] != w || g_fb_h[i] != h) {
        free(g_fb_px[i]);
        g_fb_px[i] = (uint32_t *)calloc((size_t)w * h, sizeof(uint32_t));
    }
    g_fb_w[i] = w, g_fb_h[i] = h;
    return g_fb_px[i];
}

static void convert(const uint8_t *px, int w, int h, int pitch, int bpp, const uint32_t *pal, int hd) {
    uint32_t *out = fb_px(w, h);
    if (!out) return;
    if (hd) {                                            /* the compose hook filled it */
        memcpy(out, g_hd, (size_t)w * h * sizeof(uint32_t));
    } else if (bpp == 8) {
        for (int y = 0; y < h; y++) {
            const uint8_t *row = px + (size_t)y * pitch;
            uint32_t *o = out + (size_t)y * w;
            for (int x = 0; x < w; x++) {
                uint32_t e = pal ? pal[row[x]] : 0;      /* PALETTEENTRY r, g, b */
                o[x] = 0xFF000000u | (e & 0xFF) << 16 | (e & 0xFF00) | (e >> 16 & 0xFF);
            }
        }
    } else if (bpp == 16) {
        for (int y = 0; y < h; y++) {
            const uint8_t *row = px + (size_t)y * pitch;
            uint32_t *o = out + (size_t)y * w;
            for (int x = 0; x < w; x++) {
                uint32_t p = (uint32_t)row[x * 2] | (uint32_t)row[x * 2 + 1] << 8;  /* RGB565 */
                o[x] = 0xFF000000u | ((p >> 0 & 31) << 3) << 16 | ((p >> 5 & 63) << 2) << 8 | ((p >> 11 & 31) << 3);
            }
        }
    } else {                                             /* 32: XRGB8888, as SDL's RGB888 */
        for (int y = 0; y < h; y++) {
            const uint8_t *row = px + (size_t)y * pitch;
            uint32_t *o = out + (size_t)y * w;
            for (int x = 0; x < w; x++) {
                uint32_t p = (uint32_t)row[x * 4] | (uint32_t)row[x * 4 + 1] << 8
                           | (uint32_t)row[x * 4 + 2] << 16 | (uint32_t)row[x * 4 + 3] << 24;
                o[x] = 0xFF000000u | (p & 0xFFFFFFu);
            }
        }
    }
}

static void present(void) {
    const uint8_t *px;
    const uint32_t *pal;
    int w, h, pitch, bpp;
    unsigned seq;
    if (!hle_dd_frame(&px, &w, &h, &pitch, &bpp, &pal, &seq)) return;
    uint32_t now = hle_ticks_ms();
    if (seq == g_shown_seq && now - g_last_present < 250) return;
    if (now - g_last_present < 15) return;
    g_shown_seq = seq, g_last_present = now;
    if (hle_screen_frame_hook) hle_screen_frame_hook(px, w, h, pitch, bpp);

    int hd = 0;
    if (bpp == 16 && hle_screen_compose_hook) {
        if (g_hd_n < (size_t)w * h) { free(g_hd); g_hd_n = (size_t)w * h; g_hd = (uint32_t *)malloc(g_hd_n * sizeof(uint32_t)); }
        hd = g_hd && hle_screen_compose_hook(px, pitch, w, h, g_hd);
    }
    convert(px, w, h, pitch, bpp, pal, hd);
    pthread_mutex_lock(&g_fb_lock);
    layout();
    g_fb_read = g_fb_write;
    g_fb_write = (g_fb_write + 1) % FB_COUNT;
    g_fb_seq++;
    pthread_mutex_unlock(&g_fb_lock);
}

/* ---- the pump, on the guest's thread ---- */
void hle_screen_pump(void) {
    struct input e;
    while (q_pop(&e)) {
        int gx, gy;
        to_game(e.x, e.y, &gx, &gy);
        switch (e.kind) {
        case HLE_IOS_MOUSE_MOVE: hle_input_mouse(WM_MOUSEMOVE, gx, gy); break;
        case HLE_IOS_MOUSE_DOWN: hle_input_mouse(WM_LBUTTONDOWN, gx, gy); break;
        case HLE_IOS_MOUSE_UP:   hle_input_mouse(WM_LBUTTONUP, gx, gy); break;
        case HLE_IOS_MOUSE_RDOWN: hle_input_mouse(WM_RBUTTONDOWN, gx, gy); break;
        case HLE_IOS_MOUSE_RUP:   hle_input_mouse(WM_RBUTTONUP, gx, gy); break;
        /* The key hook is fed the virtual key, not an SDL scancode: there is
         * no SDL here, and SDL_SCANCODE_F9 never meant anything to it. */
        case HLE_IOS_KEY_DOWN:   hle_input_key(1, (uint32_t)e.vk, (uint32_t)e.scan);
                                 if (hle_screen_key_hook) hle_screen_key_hook((int)e.vk, 1);
                                 break;
        case HLE_IOS_KEY_UP:     hle_input_key(0, (uint32_t)e.vk, (uint32_t)e.scan);
                                 if (hle_screen_key_hook) hle_screen_key_hook((int)e.vk, 0);
                                 break;
        default: break;
        }
    }
    if (!g_headless) { cursor_update(); present(); }
}

/* ---- opening: the guest waits for the main thread's view ---- */
static pthread_mutex_t g_ready_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_ready_cond = PTHREAD_COND_INITIALIZER;
static int g_view_ready;

void hle_ios_view_created(void) {
    pthread_mutex_lock(&g_ready_lock);
    g_view_ready = 1;
    pthread_cond_broadcast(&g_ready_cond);
    pthread_mutex_unlock(&g_ready_lock);
}

void hle_ios_want_exit(void) {
    uint32_t h = hle_first_hwnd();
    if (h) hle_post_message(h, WM_CLOSE, 0, 0);
    else hle_exit(0);
}

int hle_screen_open(const char *title, int fullscreen, int headless) {
    g_headless = headless, g_fullscreen = fullscreen;
    hle_screen_title(title);
    hle_cursor_hook = warp;
    hle_set_pump_hook(hle_screen_pump);
    if (headless) return 0;

    /* The view belongs to the main thread's run loop: wait for it, but not for
     * ever, so a missing delegate fails loudly instead of hanging the game. */
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += 20;
    pthread_mutex_lock(&g_ready_lock);
    while (!g_view_ready) {
        if (pthread_cond_timedwait(&g_ready_cond, &g_ready_lock, &deadline) == ETIMEDOUT) break;
    }
    int ready = g_view_ready;
    pthread_mutex_unlock(&g_ready_lock);
    if (!ready) { fprintf(stderr, "[screen] no view after 20s\n"); return -1; }
    fprintf(stderr, "[screen] iOS, scaling %s\n", k_modes[g_mode]);
    return 0;
}