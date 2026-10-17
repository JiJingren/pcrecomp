/*
 * dsound_ios.c - DirectSound on an iOS RemoteIO audio unit:
 * IDirectSound and IDirectSoundBuffer, with a software mixer.
 *
 * A port of ../dsound.c, which does the same on SDL2 audio. The whole mixer
 * is unchanged; only the device layer differs. Where SDL opened a device and
 * called mix() from its audio thread, this opens kAudioUnitSubType_RemoteIO,
 * asks it for the hardware's own rate and format, and makes its input-scoped
 * render callback mix() directly - so the buffer cursors advance at exactly
 * the rate the hardware consumes, with no conversion in between. The audio
 * session is taken ambient (so the ringer switch still silences it) and an
 * interruption listener stops and restarts the unit, so a phone call does
 * not leave the game silent afterwards.
 */
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <pthread.h>
#include <AudioToolbox/AudioToolbox.h>
#include <AudioUnit/AudioUnit.h>
#include <CoreAudio/CoreAudioTypes.h>
#include "win32hle.h"

#define DS_OK               0u
#define DSERR_INVALIDPARAM  0x80070057u
#define DSERR_NODRIVER      0x88780078u
#define DSERR_INVALIDCALL   0x88780032u
#define DSERR_PRIOLEVELNEEDED 0x88780046u
#define DSERR_BUFFERLOST    0x88780096u
#define DSBCAPS_PRIMARYBUFFER 0x00000001u
#define DSBPLAY_LOOPING     0x00000001u
#define DSBSTATUS_PLAYING   0x00000001u
#define DSBSTATUS_LOOPING   0x00000004u

#define OUT_RATE 44100

typedef struct {
    uint8_t *data;              /* the samples, guest-visible */
    int *data_refs;             /* shared with duplicates */
    uint32_t bytes, flags;
    int channels, bits, rate, block;
    int playing, looping, primary;
    double pos;                 /* the play cursor, in frames */
    int volume, pan;            /* hundredths of a dB; -10000..10000 */
    float gl, gr;
} dsbuf;

static pthread_mutex_t g_mix = PTHREAD_MUTEX_INITIALIZER;
#define MAX_BUFS 256
static dsbuf *g_buf[MAX_BUFS];
static AudioUnit g_au;                  /* the RemoteIO unit; 0 until it opens */
static int g_audio_ok = -1;
static double g_rate = OUT_RATE;        /* what the hardware asked for */
static float g_master = 1.0f;
void hle_dsound_set_master(float gain) { g_master = gain; }

static void gains(dsbuf *b) {
    float v = b->volume <= -10000 ? 0.0f : powf(10.0f, (float)b->volume / 2000.0f);
    float l = 1.0f, r = 1.0f;
    if (b->pan < 0) r = b->pan <= -10000 ? 0.0f : powf(10.0f, (float)b->pan / 2000.0f);
    if (b->pan > 0) l = b->pan >= 10000 ? 0.0f : powf(10.0f, (float)-b->pan / 2000.0f);
    b->gl = v * l, b->gr = v * r;
}

static inline float sample(const dsbuf *b, uint32_t frame, int ch) {
    const uint8_t *p = b->data + (size_t)frame * (uint32_t)b->block + (b->channels > 1 ? (size_t)ch * (uint32_t)(b->bits / 8) : 0);
    return b->bits == 16 ? (float)*(const int16_t *)p / 32768.0f : ((float)*p - 128.0f) / 128.0f;
}

static int audio_open(void);
static int audio_open_once(void) { return audio_open(); }

/* Host streams: PCM a host pushes (a movie's sound), mixed with the
 * buffers. A ring of interleaved s16 at the stream's rate, played at that
 * rate. */
#define MAX_STREAMS 4
#define STREAM_FRAMES (1 << 17)
static struct { int used, channels, rate; float gain; int16_t *ring; uint32_t rd, wr; double pos; } g_stream[MAX_STREAMS];
int hle_audio_stream_open(int rate, int channels) {
    if (!audio_open_once()) return -1;
    pthread_mutex_lock(&g_mix);
    int id = -1;
    for (int i = 0; i < MAX_STREAMS && id < 0; i++)
        if (!g_stream[i].used) {
            id = i;
            g_stream[i].ring = (int16_t *)calloc(STREAM_FRAMES * 2, 2);
            g_stream[i].used = 1, g_stream[i].channels = channels, g_stream[i].rate = rate, g_stream[i].gain = 1.0f;
            g_stream[i].rd = g_stream[i].wr = 0, g_stream[i].pos = 0;
        }
    pthread_mutex_unlock(&g_mix);
    return id;
}
void hle_audio_stream_push(int id, const int16_t *pcm, int frames) {
    if (id < 0 || id >= MAX_STREAMS) return;
    pthread_mutex_lock(&g_mix);
    for (int f = 0; f < frames && g_stream[id].wr - g_stream[id].rd < STREAM_FRAMES - 1; f++, g_stream[id].wr++) {
        uint32_t k = g_stream[id].wr % STREAM_FRAMES;
        g_stream[id].ring[2 * k] = pcm[f * g_stream[id].channels];
        g_stream[id].ring[2 * k + 1] = pcm[f * g_stream[id].channels + (g_stream[id].channels > 1)];
    }
    pthread_mutex_unlock(&g_mix);
}
int hle_audio_stream_queued(int id) {                    /* frames not yet played */
    if (id < 0 || id >= MAX_STREAMS) return 0;
    pthread_mutex_lock(&g_mix);
    int n = (int)(g_stream[id].wr - g_stream[id].rd);
    pthread_mutex_unlock(&g_mix);
    return n;
}
void hle_audio_stream_gain(int id, float gain) { if (id >= 0 && id < MAX_STREAMS) g_stream[id].gain = gain; }
void hle_audio_stream_close(int id) {
    if (id < 0 || id >= MAX_STREAMS) return;
    pthread_mutex_lock(&g_mix);
    free(g_stream[id].ring);
    memset(&g_stream[id], 0, sizeof g_stream[id]);
    pthread_mutex_unlock(&g_mix);
}

/* The RemoteIO input-scope render callback: the mixer, called by the audio
 * hardware for the frames it wants next, straight into its buffer. */
static OSStatus mix(void *ud, AudioUnitRenderActionFlags *flags, const AudioTimeStamp *ts,
                    UInt32 bus, UInt32 nframes, AudioBufferList *abl) {
    (void)ud; (void)flags; (void)ts; (void)bus;
    if (!abl || abl->mNumberBuffers < 1 || !abl->mBuffers[0].mData) return noErr;
    AudioBuffer *ob = &abl->mBuffers[0];
    int frames = (int)(ob->mDataByteSize / 4);            /* interleaved stereo f32 */
    if (frames > (int)nframes && nframes) frames = (int)nframes;
    if (frames > 8192) frames = 8192;
    if (frames <= 0) return noErr;
    float *stream = (float *)ob->mData;
    int split = abl->mNumberBuffers >= 2 && abl->mBuffers[1].mData != NULL;
    if (split && (int)(abl->mBuffers[1].mDataByteSize / 4) < frames)
        frames = (int)(abl->mBuffers[1].mDataByteSize / 4);
    if (frames <= 0) return noErr;
    float *stream2 = split ? (float *)abl->mBuffers[1].mData : NULL;
    static float acc[2 * 8192];
    memset(acc, 0, sizeof(float) * 2 * (size_t)frames);
    pthread_mutex_lock(&g_mix);
    for (int i = 0; i < MAX_BUFS; i++) {
        dsbuf *b = g_buf[i];
        if (!b || !b->playing || b->primary || !b->block) continue;
        uint32_t nframes_b = b->bytes / (uint32_t)b->block;
        double step = (double)b->rate / g_rate;
        for (int f = 0; f < frames && b->playing; f++) {
            uint32_t at = (uint32_t)b->pos;
            if (at >= nframes_b) {
                if (b->looping) { b->pos -= nframes_b; at = (uint32_t)b->pos; if (at >= nframes_b) b->pos = 0, at = 0; }
                else { b->playing = 0; b->pos = 0; break; }
            }
            float l = sample(b, at, 0), r = b->channels > 1 ? sample(b, at, 1) : l;
            acc[2 * f] += l * b->gl;
            acc[2 * f + 1] += r * b->gr;
            b->pos += step;
        }
    }
    for (int i = 0; i < MAX_STREAMS; i++) {
        if (!g_stream[i].used) continue;
        double step = (double)g_stream[i].rate / g_rate;
        for (int f = 0; f < frames && g_stream[i].rd < g_stream[i].wr; f++) {
            uint32_t k = g_stream[i].rd % STREAM_FRAMES;
            acc[2 * f] += g_stream[i].ring[2 * k] / 32768.0f * g_stream[i].gain;
            acc[2 * f + 1] += g_stream[i].ring[2 * k + 1] / 32768.0f * g_stream[i].gain;
            g_stream[i].pos += step;
            while (g_stream[i].pos >= 1.0 && g_stream[i].rd < g_stream[i].wr) g_stream[i].pos -= 1.0, g_stream[i].rd++;
        }
    }
    pthread_mutex_unlock(&g_mix);
    float m = g_master;
    for (int k = 0; k < 2 * frames; k++) {
        float v = acc[k] * m;
        if (v > 1.0f) v = 1.0f; else if (v < -1.0f) v = -1.0f;
        acc[k] = v;
    }
    if (split) for (int f = 0; f < frames; f++) stream[f] = acc[2 * f], stream2[f] = acc[2 * f + 1];
    else for (int f = 0; f < 2 * frames; f++) stream[f] = acc[f];
    return noErr;
}

/* The audio session: ambient (the ringer switch still silences the game), and
 * an interruption listener so a phone call does not leave it silent after. */
static void interrupted(void *ud, UInt32 state) {
    (void)ud;
    if (!g_au) return;
    if (state == kAudioSessionBeginInterruption) AudioOutputUnitStop(g_au);
    else {
        AudioSessionSetActive(true);
        AudioOutputUnitStart(g_au);
    }
}
static void session_open(void) {
    static int done;
    if (done) return;
    done = 1;
    AudioSessionInitialize(NULL, NULL, interrupted, NULL);
    UInt32 cat = kAudioSessionCategory_AmbientSound;   /* the ringer switch still silences it */
    AudioSessionSetProperty(kAudioSessionProperty_AudioCategory, sizeof cat, &cat);
}

static int audio_open(void) {
    if (g_audio_ok >= 0) return g_audio_ok;
    g_audio_ok = 0;
    session_open();
    AudioComponentDescription desc;
    memset(&desc, 0, sizeof desc);
    desc.componentType = kAudioUnitType_Output;
    desc.componentSubType = kAudioUnitSubType_RemoteIO;
    desc.componentManufacturer = kAudioUnitManufacturer_Apple;
    AudioComponent comp = AudioComponentFindNext(NULL, &desc);
    if (!comp || AudioComponentInstanceNew(comp, &g_au) != noErr) {
        g_au = 0;
        fprintf(stderr, "[dsound] no RemoteIO audio unit\n");
        return 0;
    }
    /* What the hardware wants: matching it exactly leaves the converter out of
     * the path, so the mixer's output lands on the speaker unmodified. */
    AudioStreamBasicDescription hw;
    UInt32 sz = sizeof hw;
    if (AudioUnitGetProperty(g_au, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Output, 0, &hw, &sz) != noErr ||
        hw.mSampleRate <= 0 || hw.mChannelsPerFrame < 1) {
        hw.mSampleRate = OUT_RATE;
        hw.mChannelsPerFrame = 2;
    }
    g_rate = (double)hw.mSampleRate;
    int nch = hw.mChannelsPerFrame > 2 ? 2 : hw.mChannelsPerFrame;
    AudioStreamBasicDescription want;
    memset(&want, 0, sizeof want);
    want.mSampleRate = hw.mSampleRate;
    want.mFormatID = kAudioFormatLinearPCM;
    want.mFormatFlags = kAudioFormatFlagsNativeFloatPacked;
    want.mBytesPerPacket = (UInt32)(4 * nch);
    want.mFramesPerPacket = 1;
    want.mBytesPerFrame = (UInt32)(4 * nch);
    want.mChannelsPerFrame = (UInt32)nch;
    want.mBitsPerChannel = 32;
    if (AudioUnitSetProperty(g_au, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0, &want, sizeof want) != noErr) {
        fprintf(stderr, "[dsound] the audio unit refused %.0f Hz float PCM\n", g_rate);
        AudioComponentInstanceDispose(g_au);
        g_au = 0;
        return 0;
    }
    AURenderCallbackStruct cb;
    memset(&cb, 0, sizeof cb);
    cb.inputProc = mix, cb.inputProcRefCon = NULL;
    if (AudioUnitSetProperty(g_au, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, 0, &cb, sizeof cb) != noErr ||
        AudioUnitInitialize(g_au) != noErr || AudioSessionSetActive(true) != noErr ||
        AudioOutputUnitStart(g_au) != noErr) {
        fprintf(stderr, "[dsound] the audio unit would not start\n");
        if (g_au) AudioComponentInstanceDispose(g_au);
        g_au = 0;
        return 0;
    }
    fprintf(stderr, "[dsound] RemoteIO, %.0f Hz, %d ch\n", g_rate, nch);
    return g_audio_ok = 1;
}

/* the object: vtable, refs, then the buffer slot */
#define SLOT(self) MEM32((self) + 8)
static dsbuf *buf_of(uint32_t self) { uint32_t i = SLOT(self); return i < MAX_BUFS ? g_buf[i] : NULL; }

static uint32_t g_vt_ds, g_vt_buf;
static uint32_t new_buffer(dsbuf *b) {
    pthread_mutex_lock(&g_mix);
    int slot = -1;
    for (int i = 0; i < MAX_BUFS && slot < 0; i++) if (!g_buf[i]) slot = i;
    if (slot >= 0) g_buf[slot] = b;
    pthread_mutex_unlock(&g_mix);
    if (slot < 0) return 0;
    uint32_t o = hle_com_new(g_vt_buf, 12);
    SLOT(o) = (uint32_t)slot;
    return o;
}

/* ---- IDirectSoundBuffer ---- */
static void b_Release(void) {
    uint32_t self = A32(0), n = --MEM32(self + 4);
    if (!n) {
        pthread_mutex_lock(&g_mix);
        dsbuf *b = buf_of(self);
        g_buf[SLOT(self)] = NULL;
        pthread_mutex_unlock(&g_mix);
        if (b) {
            if (--*b->data_refs == 0) { hle_free(b->data); free(b->data_refs); }
            free(b);
        }
        free((void *)(uintptr_t)self);
    }
    RET(n, 1);
}
static void b_GetCaps(void) {                            /* (this, &DSBCAPS) */
    dsbuf *b = buf_of(A32(0));
    uint32_t c = A32(1);
    if (!b || !c) RET(DSERR_INVALIDPARAM, 2);
    MEM32(c + 4) = b->flags | 0x8u /* LOCSOFTWARE */, MEM32(c + 8) = b->bytes, MEM32(c + 12) = 0, MEM32(c + 16) = 0;
    RET(DS_OK, 2);
}
static void b_GetCurrentPosition(void) {                 /* (this, &play, &write) */
    dsbuf *b = buf_of(A32(0));
    if (!b) RET(DSERR_INVALIDPARAM, 3);
    pthread_mutex_lock(&g_mix);
    uint32_t play = b->block ? (uint32_t)b->pos * (uint32_t)b->block : 0;
    uint32_t lead = b->block ? (uint32_t)(b->rate / 100) * (uint32_t)b->block : 0;   /* 10 ms ahead */
    pthread_mutex_unlock(&g_mix);
    if (b->bytes) play %= b->bytes;
    if (A32(1)) MEM32(A32(1)) = play;
    if (A32(2)) MEM32(A32(2)) = b->bytes ? (play + (b->playing ? lead : 0)) % b->bytes : 0;
    RET(DS_OK, 3);
}
static void b_GetFormat(void) {                          /* (this, &wfx, size, &written) */
    dsbuf *b = buf_of(A32(0));
    uint32_t w = A32(1);
    if (!b) RET(DSERR_INVALIDPARAM, 4);
    if (w && A32(2) >= 16) {
        MEM16(w) = 1, MEM16(w + 2) = (uint16_t)b->channels, MEM32(w + 4) = (uint32_t)b->rate;
        MEM32(w + 8) = (uint32_t)(b->rate * b->block), MEM16(w + 12) = (uint16_t)b->block, MEM16(w + 14) = (uint16_t)b->bits;
        if (A32(2) >= 18) MEM16(w + 16) = 0;
    }
    if (A32(3)) MEM32(A32(3)) = 18;
    RET(DS_OK, 4);
}
static void b_GetVolume(void)    { dsbuf *b = buf_of(A32(0)); if (A32(1) && b) MEM32(A32(1)) = (uint32_t)b->volume; RET(b ? DS_OK : DSERR_INVALIDPARAM, 2); }
static void b_GetPan(void)       { dsbuf *b = buf_of(A32(0)); if (A32(1) && b) MEM32(A32(1)) = (uint32_t)b->pan; RET(b ? DS_OK : DSERR_INVALIDPARAM, 2); }
static void b_GetFrequency(void) { dsbuf *b = buf_of(A32(0)); if (A32(1) && b) MEM32(A32(1)) = (uint32_t)b->rate; RET(b ? DS_OK : DSERR_INVALIDPARAM, 2); }
static void b_GetStatus(void) {
    dsbuf *b = buf_of(A32(0));
    if (A32(1) && b) MEM32(A32(1)) = (b->playing ? DSBSTATUS_PLAYING : 0u) | (b->playing && b->looping ? DSBSTATUS_LOOPING : 0u);
    RET(b ? DS_OK : DSERR_INVALIDPARAM, 2);
}
static void b_Initialize(void) { RET(DSERR_INVALIDCALL, 3); }   /* already initialized */
static void b_Lock(void) {                               /* (this, offset, bytes, &p1, &n1, &p2, &n2, flags) */
    dsbuf *b = buf_of(A32(0));
    if (!b || b->primary) RET(b ? DSERR_PRIOLEVELNEEDED : DSERR_INVALIDPARAM, 8);
    uint32_t off = A32(1), n = A32(2), fl = A32(7);
    if (fl & 2u) off = 0, n = b->bytes;                  /* DSBLOCK_ENTIREBUFFER */
    else if (fl & 1u) {                                  /* DSBLOCK_FROMWRITECURSOR */
        pthread_mutex_lock(&g_mix);
        off = b->block ? ((uint32_t)b->pos * (uint32_t)b->block + (uint32_t)(b->rate / 100) * (uint32_t)b->block) % b->bytes : 0;
        pthread_mutex_unlock(&g_mix);
    }
    if (off >= b->bytes || n > b->bytes) RET(DSERR_INVALIDPARAM, 8);
    uint32_t n1 = off + n <= b->bytes ? n : b->bytes - off;
    if (A32(3)) MEM32(A32(3)) = (uint32_t)(uintptr_t)(b->data + off);
    if (A32(4)) MEM32(A32(4)) = n1;
    if (A32(5)) MEM32(A32(5)) = n1 < n ? (uint32_t)(uintptr_t)b->data : 0;
    if (A32(6)) MEM32(A32(6)) = n - n1;
    RET(DS_OK, 8);
}
static void b_Unlock(void) { RET(DS_OK, 5); }            /* (this, p1, n1, p2, n2): the memory is the buffer */
static void b_Play(void) {                               /* (this, reserved, priority, flags) */
    dsbuf *b = buf_of(A32(0));
    if (!b) RET(DSERR_INVALIDPARAM, 4);
    pthread_mutex_lock(&g_mix);
    b->playing = 1, b->looping = (A32(3) & DSBPLAY_LOOPING) != 0;
    pthread_mutex_unlock(&g_mix);
    RET(DS_OK, 4);
}
static void b_SetCurrentPosition(void) {
    dsbuf *b = buf_of(A32(0));
    if (!b || !b->block) RET(DSERR_INVALIDPARAM, 2);
    pthread_mutex_lock(&g_mix);
    b->pos = (double)((A32(1) % (b->bytes ? b->bytes : 1)) / (uint32_t)b->block);
    pthread_mutex_unlock(&g_mix);
    RET(DS_OK, 2);
}
static void b_SetFormat(void) {                          /* the primary's: the mix is at the device rate anyway */
    dsbuf *b = buf_of(A32(0));
    uint32_t w = A32(1);
    if (b && w) b->channels = MEM16(w + 2), b->rate = (int)MEM32(w + 4), b->bits = MEM16(w + 14), b->block = MEM16(w + 12);
    RET(DS_OK, 2);
}
static void b_SetVolume(void) {
    dsbuf *b = buf_of(A32(0));
    if (!b) RET(DSERR_INVALIDPARAM, 2);
    pthread_mutex_lock(&g_mix);
    b->volume = (int)A32(1);
    gains(b);
    pthread_mutex_unlock(&g_mix);
    RET(DS_OK, 2);
}
static void b_SetPan(void) {
    dsbuf *b = buf_of(A32(0));
    if (!b) RET(DSERR_INVALIDPARAM, 2);
    pthread_mutex_lock(&g_mix);
    b->pan = (int)A32(1);
    gains(b);
    pthread_mutex_unlock(&g_mix);
    RET(DS_OK, 2);
}
static void b_SetFrequency(void) {
    dsbuf *b = buf_of(A32(0));
    if (!b) RET(DSERR_INVALIDPARAM, 2);
    pthread_mutex_lock(&g_mix);
    if (A32(1)) b->rate = (int)A32(1);
    pthread_mutex_unlock(&g_mix);
    RET(DS_OK, 2);
}
static void b_Stop(void) {
    dsbuf *b = buf_of(A32(0));
    if (!b) RET(DSERR_INVALIDPARAM, 1);
    pthread_mutex_lock(&g_mix);
    b->playing = 0;
    pthread_mutex_unlock(&g_mix);
    RET(DS_OK, 1);
}
static void b_Restore(void) { RET(DS_OK, 1); }

/* ---- IDirectSound ---- */
static void d_Release(void) {
    uint32_t self = A32(0), n = --MEM32(self + 4);
    if (!n) free((void *)(uintptr_t)self);
    RET(n, 1);
}
static void d_CreateSoundBuffer(void) {                  /* (this, &DSBUFFERDESC, &out, outer) */
    uint32_t d = A32(1), out = A32(2);
    if (!d || !out) RET(DSERR_INVALIDPARAM, 4);
    uint32_t flags = MEM32(d + 4), bytes = MEM32(d + 8), wfx = MEM32(d + 16);
    dsbuf *b = (dsbuf *)calloc(1, sizeof *b);
    b->flags = flags, b->primary = (flags & DSBCAPS_PRIMARYBUFFER) != 0;
    b->channels = 2, b->bits = 16, b->rate = 22050, b->block = 4;
    if (wfx) {
        b->channels = MEM16(wfx + 2), b->rate = (int)MEM32(wfx + 4), b->block = MEM16(wfx + 12), b->bits = MEM16(wfx + 14);
        if (MEM16(wfx) != 1 || (b->bits != 8 && b->bits != 16) || b->channels < 1 || b->channels > 2) {
            fprintf(stderr, "[dsound] buffer format tag %u, %d bits, %d channels: not PCM this mixer plays\n", MEM16(wfx), b->bits, b->channels);
            free(b);
            RET(DSERR_INVALIDPARAM, 4);
        }
    }
    if (b->primary) bytes = 4096 * 4;
    b->bytes = bytes;
    b->data = (uint8_t *)hle_alloc(bytes ? bytes : 4);
    if (b->bits == 8) memset(b->data, 128, bytes);
    b->data_refs = (int *)malloc(sizeof(int));
    *b->data_refs = 1;
    gains(b);
    uint32_t o = new_buffer(b);
    if (!o) { hle_free(b->data); free(b->data_refs); free(b); RET(DSERR_INVALIDPARAM, 4); }
    MEM32(out) = o;
    RET(DS_OK, 4);
}
static void d_GetCaps(void) {                            /* (this, &DSCAPS) */
    uint32_t c = A32(1);
    if (c) {
        uint32_t size = MEM32(c);
        memset((void *)(uintptr_t)(c + 4), 0, size > 4 ? size - 4 : 0);
        MEM32(c + 4) = 0x0F5Fu;                          /* the primary/secondary 8/16-bit mono/stereo, continuous rate */
        MEM32(c + 8) = 100, MEM32(c + 12) = 100000;      /* min/max secondary rate */
        MEM32(c + 16) = 1;                               /* primary buffers */
        MEM32(c + 20) = 64, MEM32(c + 24) = 64, MEM32(c + 28) = 64;   /* max hw mixing: all, static, streaming */
        MEM32(c + 32) = 64, MEM32(c + 36) = 64, MEM32(c + 40) = 64;   /* free hw */
    }
    RET(DS_OK, 2);
}
static void d_DuplicateSoundBuffer(void) {               /* (this, original, &out) */
    dsbuf *src = buf_of(A32(1));
    if (!src || !A32(2)) RET(DSERR_INVALIDPARAM, 3);
    dsbuf *b = (dsbuf *)malloc(sizeof *b);
    pthread_mutex_lock(&g_mix);
    *b = *src;
    (*b->data_refs)++;
    pthread_mutex_unlock(&g_mix);
    b->playing = 0, b->pos = 0;
    uint32_t o = new_buffer(b);
    MEM32(A32(2)) = o;
    RET(o ? DS_OK : DSERR_INVALIDPARAM, 3);
}
static void d_SetCooperativeLevel(void) { RET(DS_OK, 3); }
static void d_Compact(void) { RET(DS_OK, 1); }
static void d_GetSpeakerConfig(void) { if (A32(1)) MEM32(A32(1)) = 4; RET(DS_OK, 2); }   /* DSSPEAKER_STEREO */
static void d_SetSpeakerConfig(void) { RET(DS_OK, 2); }
static void d_Initialize(void) { RET(DS_OK, 2); }

static void vtables(void) {
    if (g_vt_ds) return;
    static const win32hle_shim ds[] = {
        { "IDirectSound::QueryInterface", hle_com_QueryInterface }, { "IDirectSound::AddRef", hle_com_AddRef },
        { "IDirectSound::Release", d_Release }, { "IDirectSound::CreateSoundBuffer", d_CreateSoundBuffer },
        { "IDirectSound::GetCaps", d_GetCaps }, { "IDirectSound::DuplicateSoundBuffer", d_DuplicateSoundBuffer },
        { "IDirectSound::SetCooperativeLevel", d_SetCooperativeLevel }, { "IDirectSound::Compact", d_Compact },
        { "IDirectSound::GetSpeakerConfig", d_GetSpeakerConfig }, { "IDirectSound::SetSpeakerConfig", d_SetSpeakerConfig },
        { "IDirectSound::Initialize", d_Initialize }, { 0, 0 } };
    static const win32hle_shim buf[] = {
        { "IDirectSoundBuffer::QueryInterface", hle_com_QueryInterface }, { "IDirectSoundBuffer::AddRef", hle_com_AddRef },
        { "IDirectSoundBuffer::Release", b_Release }, { "IDirectSoundBuffer::GetCaps", b_GetCaps },
        { "IDirectSoundBuffer::GetCurrentPosition", b_GetCurrentPosition }, { "IDirectSoundBuffer::GetFormat", b_GetFormat },
        { "IDirectSoundBuffer::GetVolume", b_GetVolume }, { "IDirectSoundBuffer::GetPan", b_GetPan },
        { "IDirectSoundBuffer::GetFrequency", b_GetFrequency }, { "IDirectSoundBuffer::GetStatus", b_GetStatus },
        { "IDirectSoundBuffer::Initialize", b_Initialize }, { "IDirectSoundBuffer::Lock", b_Lock },
        { "IDirectSoundBuffer::Play", b_Play }, { "IDirectSoundBuffer::SetCurrentPosition", b_SetCurrentPosition },
        { "IDirectSoundBuffer::SetFormat", b_SetFormat }, { "IDirectSoundBuffer::SetVolume", b_SetVolume },
        { "IDirectSoundBuffer::SetPan", b_SetPan }, { "IDirectSoundBuffer::SetFrequency", b_SetFrequency },
        { "IDirectSoundBuffer::Stop", b_Stop }, { "IDirectSoundBuffer::Unlock", b_Unlock },
        { "IDirectSoundBuffer::Restore", b_Restore }, { 0, 0 } };
    g_vt_ds = hle_com_vtable(ds);
    g_vt_buf = hle_com_vtable(buf);
}

static void s_DirectSoundCreate(void) {                  /* (guid, &out, outer) */
    vtables();
    if (!A32(1)) RET(DSERR_INVALIDPARAM, 3);
    if (!audio_open()) { MEM32(A32(1)) = 0; RET(DSERR_NODRIVER, 3); }
    MEM32(A32(1)) = hle_com_new(g_vt_ds, 12);
    RET(DS_OK, 3);
}
static void s_DirectSoundEnumerateA(void) {              /* (callback, context): the one device */
    static char desc[] = "Primary Sound Driver", mod[] = "";
    uint32_t a[4] = { 0, (uint32_t)(uintptr_t)desc, (uint32_t)(uintptr_t)mod, A32(1) };
    if (A32(0)) hle_call_guest(A32(0), 4, a);
    RET(DS_OK, 2);
}

const win32hle_shim win32hle_dsound[] = {
    { "dsound.dll#1", s_DirectSoundCreate }, { "DirectSoundCreate", s_DirectSoundCreate },
    { "dsound.dll#2", s_DirectSoundEnumerateA }, { "DirectSoundEnumerateA", s_DirectSoundEnumerateA },
    { 0, 0 }
};
