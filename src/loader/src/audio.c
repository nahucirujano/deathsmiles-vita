// Audio: minimal OpenAL (software mixer on sceAudioOut), AudioSession and ExtAudioFile (PCM WAV).
// Budget: mixing must cost < 1 ms per 16.7 ms frame; it is measured and logged.
#include <stdlib.h>
#include <string.h>
#include <psp2/audioout.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/io/fcntl.h>
#include "objc.h"

#define OUT_RATE 48000
#define GRAIN 512                     // 10.7 ms per block
#define MAXBUF 512
#define MAXSRC 512
typedef struct { int used; int16_t *pcm; uint32_t frames; int ch; uint32_t rate; uint32_t name; } ALBuf;
typedef struct { int used; uint32_t buf; int state, loop; float gain, pitch; uint64_t pos; /* 32.32 */ int has_pending; uint64_t pending; uint32_t name; uint32_t play_gen; } ALSrc;
static ALBuf g_buf[MAXBUF]; static ALSrc g_src[MAXSRC];
static SceKernelLwMutexWork g_amx;
static int g_audio_started;
static volatile uint32_t g_play_gen;   // increases with every alSourcePlay (music segment chaining)
enum { AL_INITIAL = 0x1011, AL_PLAYING, AL_PAUSED, AL_STOPPED };
#define LOCK() sceKernelLockLwMutex(&g_amx, 1, NULL)
#define UNLOCK() sceKernelUnlockLwMutex(&g_amx, 1)

static uint32_t g_chain_ok, g_chain_late; static uint64_t g_chain_wait_max;
static int mixer_thread(SceSize n, void *p) {
  int port = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_BGM, GRAIN, OUT_RATE, SCE_AUDIO_OUT_MODE_STEREO);
  if (port < 0) { dslog("[CPU-bound] [error] sceAudioOutOpenPort=%08x", port); return 0; }
  static int16_t out[2][GRAIN * 2]; static int32_t acc[GRAIN * 2];
  int cur = 0; uint64_t busy = 0, blocks = 0;
  for (;;) {
    uint64_t t0 = sceKernelGetProcessTimeWide();
    memset(acc, 0, sizeof acc);
    LOCK();
    uint32_t gen0 = g_play_gen; int end_at = GRAIN;   // first frame where a long segment (music) ended
    for (int s = 0; s < MAXSRC; s++) {
      ALSrc *src = &g_src[s];
      if (!src->used || src->state != AL_PLAYING || !src->buf) continue;
      ALBuf *b = &g_buf[src->buf - 1];
      if (!b->used || !b->pcm || !b->frames) { src->state = AL_STOPPED; continue; }
      uint64_t step = ((uint64_t)b->rate << 32) / OUT_RATE;
      step = (uint64_t)(step * src->pitch);
      int g = (int)(src->gain * 256);
      for (int i = 0; i < GRAIN; i++) {
        uint32_t f = src->pos >> 32;
        if (f >= b->frames) {
          if (src->loop) { src->pos -= (uint64_t)b->frames << 32; f = src->pos >> 32; }
          else { src->state = AL_STOPPED; if (b->frames > b->rate && i < end_at) end_at = i; break; }
        }
        int l, r;
        if (b->ch == 2) { l = b->pcm[f * 2]; r = b->pcm[f * 2 + 1]; } else l = r = b->pcm[f];
        acc[i * 2] += (l * g) >> 8; acc[i * 2 + 1] += (r * g) >> 8;
        src->pos += step;
      }
    }
    UNLOCK();
    // The music of cutscenes and credits is made of segments chained by the game's -[ES1Renderer onBgmSeqThread]
    // thread (it polls the state every 1 ms and starts the next segment when the previous one stops). Before,
    // the rest of the 10.7 ms block stayed silent and the new segment started on the next block:
    // gaps of up to ~21 ms at every join = stuttering audio. Now, if a long segment ended inside the
    // block, the mixer waits up to 4 ms for the game to start the next one and mixes it from the exact end frame.
    if (end_at < GRAIN) {
      uint64_t w0 = sceKernelGetProcessTimeWide(), w = 0;
      while (g_play_gen == gen0 && (w = sceKernelGetProcessTimeWide() - w0) < 4000) sceKernelDelayThread(200);
      if (g_play_gen != gen0) {
        LOCK();
        for (int s = 0; s < MAXSRC; s++) {
          ALSrc *src = &g_src[s];
          if (!src->used || src->state != AL_PLAYING || !src->buf || (int32_t)(src->play_gen - gen0) <= 0) continue;
          ALBuf *b = &g_buf[src->buf - 1];
          if (!b->used || !b->pcm || !b->frames) continue;
          uint64_t step = (uint64_t)((((uint64_t)b->rate << 32) / OUT_RATE) * src->pitch);
          int g = (int)(src->gain * 256);
          for (int i = end_at; i < GRAIN; i++) {
            uint32_t f = src->pos >> 32;
            if (f >= b->frames) { if (src->loop) { src->pos -= (uint64_t)b->frames << 32; f = src->pos >> 32; } else { src->state = AL_STOPPED; break; } }
            int l, r;
            if (b->ch == 2) { l = b->pcm[f * 2]; r = b->pcm[f * 2 + 1]; } else l = r = b->pcm[f];
            acc[i * 2] += (l * g) >> 8; acc[i * 2 + 1] += (r * g) >> 8;
            src->pos += step;
          }
          src->play_gen = gen0;   // already mixed in this block: continues normally in the next one
        }
        UNLOCK();
        g_chain_ok++; if (w > g_chain_wait_max) g_chain_wait_max = w;
      } else g_chain_late++;
      t0 += w;   // the wait does not count as mixing cost
    }
    for (int i = 0; i < GRAIN * 2; i++) { int v = acc[i]; out[cur][i] = v > 32767 ? 32767 : v < -32768 ? -32768 : v; }
    busy += sceKernelGetProcessTimeWide() - t0;
    if (++blocks == 470) {   // ~5 s
      float ms_per_frame = busy / 1000.0f / blocks * (16.667f / 10.667f);
      if (DS_VERBOSE || ms_per_frame > 1.0f) dslog("%s audio: %.3f ms per 16.7 ms frame (limit 1 ms)%s", ms_per_frame > 1.0f ? "[CPU-bound]" : "[OK]",
            ms_per_frame, ms_per_frame > 1.0f ? " [CPU-bound: mezclado demasiado caro]" : "");
      // "with no follow-up" = long sounds that simply end (not a gap): not a problem.
      if (g_chain_ok)
        dslogv("[OK] music segments: %u joined without gap (max wait %.1f ms), %u long sounds ended with no follow-up",
              g_chain_ok, g_chain_wait_max / 1000.0f, g_chain_late);
      g_chain_ok = g_chain_late = 0; g_chain_wait_max = 0;
      busy = blocks = 0;
    }
    sceAudioOutOutput(port, out[cur]); cur ^= 1;
  }
  return 0;
}
static void audio_start(void) {
  if (g_audio_started) return;
  g_audio_started = 1;
  SceUID t = sceKernelCreateThread("ds_audio", mixer_thread, 0x10000100 - 0x10, 0x10000, 0, 0x40000 /* core 2 */, NULL);
  sceKernelStartThread(t, 0, NULL);
  dslog("audio started: 48 kHz stereo software mixer on core 2");
}

// ---------------- ALC ----------------
static int g_dev = 1, g_ctx = 1;
static void *ios_alcOpenDevice(const char *n) { audio_start(); return &g_dev; }
static int ios_alcCloseDevice(void *d) { return 1; }
static void *ios_alcCreateContext(void *d, const int *a) { return &g_ctx; }
static int ios_alcMakeContextCurrent(void *c) { return 1; }
static void ios_alcVoid(void *c) {}
static int ios_alcGetError(void *d) { return 0; }

// ---------------- AL ----------------
static int ios_alGetError(void) { return 0; }
// Names as in Apple's OpenAL: increasing and never reused, with separate counters for
// buffers and sources that advance together. evac_sound (CAVE's sound engine) relies on this: it stores a
// single name per channel (the same for source and buffer) and queries old names with alIsSource /
// AL_SOURCE_TYPE to know whether the channel is still loaded. (Found while studying touchHLE: NameMap.)
static uint32_t g_next_buf = 1, g_next_src = 1;
static int bslot(uint32_t id) { if (!id) return 0; for (int i = 0; i < MAXBUF; i++) if (g_buf[i].used && g_buf[i].name == id) return i + 1; return 0; }
static int sslot(uint32_t id) { if (!id) return 0; for (int i = 0; i < MAXSRC; i++) if (g_src[i].used && g_src[i].name == id) return i + 1; return 0; }
static uint32_t bid(int slot) { return slot ? g_buf[slot - 1].name : 0; }
static void ios_alGenBuffers(int n, uint32_t *ids) {
  LOCK();
  for (int k = 0; k < n; k++) {
    ids[k] = 0;
    for (int i = 0; i < MAXBUF; i++) if (!g_buf[i].used) {
      memset(&g_buf[i], 0, sizeof(ALBuf)); g_buf[i].used = 1; g_buf[i].name = ids[k] = g_next_buf++; break;
    }
  }
  UNLOCK();
  for (int k = 0; k < n; k++) if (!ids[k]) dslog("[RAM-bound] [error] no free audio buffers (maximum %d)", MAXBUF);
}
static void ios_alDeleteBuffers(int n, const uint32_t *ids) {
  LOCK();
  for (int k = 0; k < n; k++) {
    int b = bslot(ids[k]); if (!b) continue;
    int busy = 0;
    for (int s = 0; s < MAXSRC; s++) if (g_src[s].used && g_src[s].buf == b && (g_src[s].state == AL_PLAYING || g_src[s].state == AL_PAUSED)) busy = 1;
    if (busy) { dslog("[warning] alDeleteBuffers on a buffer in use: rejected (as in OpenAL)"); continue; }
    for (int s = 0; s < MAXSRC; s++) if (g_src[s].used && g_src[s].buf == b) g_src[s].buf = 0;
    free(g_buf[b - 1].pcm); g_buf[b - 1].pcm = NULL; g_buf[b - 1].used = 0;
  }
  UNLOCK();
}
static int ios_alIsBuffer(uint32_t b) { return bslot(b) != 0; }
static void ios_alBufferData(uint32_t bufid, int fmt, const void *data, int size, int freq) {
  int b = bslot(bufid); if (!b) return;
  int ch = (fmt == 0x1102 || fmt == 0x1103) ? 2 : 1, bits16 = (fmt == 0x1101 || fmt == 0x1103);
  uint32_t frames = size / (ch * (bits16 ? 2 : 1));
  int16_t *pcm = malloc(frames * ch * 2);
  if (!pcm) { dslog("[RAM-bound] [error] out of memory for audio (%d bytes)", size); return; }
  if (bits16) memcpy(pcm, data, frames * ch * 2);
  else for (uint32_t i = 0; i < frames * ch; i++) pcm[i] = (((const uint8_t *)data)[i] - 128) << 8;
  LOCK(); ALBuf *B = &g_buf[b - 1]; free(B->pcm); B->pcm = pcm; B->frames = frames; B->ch = ch; B->rate = freq; UNLOCK();
}
static void ios_alGenSources(int n, uint32_t *ids) {
  LOCK();
  for (int k = 0; k < n; k++) {
    ids[k] = 0;
    for (int i = 0; i < MAXSRC; i++) if (!g_src[i].used) {
      memset(&g_src[i], 0, sizeof(ALSrc));
      g_src[i].used = 1; g_src[i].gain = g_src[i].pitch = 1; g_src[i].state = AL_INITIAL; g_src[i].name = ids[k] = g_next_src++; break;
    }
  }
  UNLOCK();
  for (int k = 0; k < n; k++) if (!ids[k]) dslog("[CPU-bound] [error] no free audio sources");
}
static void ios_alDeleteSources(int n, const uint32_t *ids) {
  LOCK(); for (int k = 0; k < n; k++) { int s = sslot(ids[k]); if (s) { g_src[s - 1].used = 0; g_src[s - 1].state = AL_STOPPED; } } UNLOCK();
}
static int ios_alIsSource(uint32_t s) { return sslot(s) != 0; }
static ALSrc *S_(uint32_t s) { int k = sslot(s); return k ? &g_src[k - 1] : NULL; }
// playback offset (AL_SEC_OFFSET 0x1024, AL_SAMPLE_OFFSET 0x1025, AL_BYTE_OFFSET 0x1026).
// The game uses it to sequence the music (intro + loop). If the source is not playing, it applies on the next Play.
static uint32_t buf_rate(ALSrc *x) { return x->buf && g_buf[x->buf - 1].rate ? g_buf[x->buf - 1].rate : 44100; }
static uint32_t buf_bpf(ALSrc *x) { return x->buf ? g_buf[x->buf - 1].ch * 2 : 2; }
static void set_offset(ALSrc *x, int p, double v) {
  uint64_t frames = p == 0x1024 ? (uint64_t)(v * buf_rate(x)) : p == 0x1025 ? (uint64_t)v : (uint64_t)v / buf_bpf(x);
  LOCK();
  if (x->state == AL_PLAYING || x->state == AL_PAUSED) x->pos = frames << 32;
  else { x->pending = frames << 32; x->has_pending = 1; }
  UNLOCK();
}
static double get_offset(ALSrc *x, int p) {
  uint64_t f = (x->state == AL_PLAYING || x->state == AL_PAUSED) ? x->pos >> 32 : 0;
  return p == 0x1024 ? (double)f / buf_rate(x) : p == 0x1025 ? (double)f : (double)f * buf_bpf(x);
}
static void ios_alSourcei(uint32_t s, int p, int v) {
  ALSrc *x = S_(s); if (!x) return;
  LOCK();
  if (p == 0x1009) {                                                   // AL_BUFFER (only while the source is stopped)
    if (x->state == AL_PLAYING || x->state == AL_PAUSED) dslog("[warning] AL_BUFFER on a playing source: rejected (as in OpenAL)");
    else { x->buf = bslot(v); x->pos = 0; x->has_pending = 0; }
  }
  else if (p == 0x1007) x->loop = v;                  // AL_LOOPING
  UNLOCK();
  if (p >= 0x1024 && p <= 0x1026) set_offset(x, p, v);
}
SOFTFP static void ios_alSourcef(uint32_t s, int p, float v) {
  ALSrc *x = S_(s); if (!x) return;
  if (p == 0x100A) x->gain = v; else if (p == 0x1003) x->pitch = v > 0.01f ? v : 0.01f;
  else if (p >= 0x1024 && p <= 0x1026) set_offset(x, p, v);
}
SOFTFP static void ios_alSource3f(uint32_t s, int p, float a, float b, float c) {}
SOFTFP static void ios_alListener3f(int p, float a, float b, float c) {}
static void ios_alListenerfv(int p, const float *v) {}
static void ios_alSourcePlay(uint32_t s) {
  ALSrc *x = S_(s); if (!x) return;
  LOCK();
  if (x->state != AL_PAUSED) { x->pos = x->has_pending ? x->pending : 0; x->has_pending = 0; }
  x->state = AL_PLAYING; x->play_gen = ++g_play_gen;
  UNLOCK();
}
static void ios_alSourceStop(uint32_t s) { ALSrc *x = S_(s); if (!x) return; LOCK(); x->state = AL_STOPPED; x->pos = 0; UNLOCK(); }
static void ios_alSourcePause(uint32_t s) { ALSrc *x = S_(s); if (!x) return; LOCK(); if (x->state == AL_PLAYING) x->state = AL_PAUSED; UNLOCK(); }
static void ios_alGetSourcei(uint32_t s, int p, int *v) {
  *v = 0;                                    // as in touchHLE: with an invalid name, the game reads 0
  ALSrc *x = S_(s); if (!x) return;
  if (p == 0x1027) { *v = x->buf ? 0x1028 /* AL_STATIC */ : 0x1030 /* AL_UNDETERMINED */; return; }   // AL_SOURCE_TYPE
  if (p == 0x1010) *v = x->state; else if (p == 0x1009) *v = bid(x->buf); else if (p == 0x1007) *v = x->loop;
  else if (p >= 0x1024 && p <= 0x1026) *v = (int)get_offset(x, p); else *v = 0;
}
static void ios_alGetSourcef(uint32_t s, int p, float *v) {
  *v = 0;
  ALSrc *x = S_(s);
  *v = !x ? 0 : p == 0x100A ? x->gain : p == 0x1003 ? x->pitch : (p >= 0x1024 && p <= 0x1026) ? (float)get_offset(x, p) : 0;
}

// ---------------- AudioSession (not needed on the Vita) ----------------
static int ios_AudioSessionZero(void) { return 0; }
static int ios_AudioSessionGetProperty(uint32_t id_, uint32_t *size, void *data) { if (data && size && *size >= 4) *(uint32_t *)data = 0; return 0; }

// ---------------- ExtAudioFile (WAV PCM 16 bits) ----------------
typedef struct { double rate; uint32_t fmt, flags, bpp, fpp, bpf, ch, bits, res; } ASBD;
typedef struct { uint8_t *data; uint32_t frames, pos; ASBD file, client; } XAF;
const char *url_path(id u);
static int ios_ExtAudioFileOpenURL(id url, XAF **out) {
  const char *p = url_path(url);
  SceUID fd = p ? sceIoOpen(p, SCE_O_RDONLY, 0) : -1;
  if (fd < 0) { dslog("[warning] ExtAudioFileOpenURL: %s not found", p ? p : "(nil)"); *out = NULL; return -43; }
  uint32_t sz = sceIoLseek(fd, 0, SCE_SEEK_END); sceIoLseek(fd, 0, SCE_SEEK_SET);
  uint8_t *f = malloc(sz); sceIoRead(fd, f, sz); sceIoClose(fd);
  XAF *x = calloc(1, sizeof(XAF));
  for (uint32_t i = 12; i + 8 <= sz;) {
    uint32_t cs = *(uint32_t *)(f + i + 4);
    if (!memcmp(f + i, "fmt ", 4)) {
      x->file.ch = *(uint16_t *)(f + i + 10); x->file.rate = *(uint32_t *)(f + i + 12); x->file.bits = *(uint16_t *)(f + i + 22);
    } else if (!memcmp(f + i, "data", 4)) {
      if (cs > sz - i - 8) cs = sz - i - 8;
      x->data = malloc(cs); memcpy(x->data, f + i + 8, cs);
      x->frames = cs / (x->file.ch * x->file.bits / 8);
    }
    i += 8 + cs + (cs & 1);
  }
  free(f);
  x->file.fmt = 0x6c70636d; x->file.flags = 12; x->file.fpp = 1; x->file.bpf = x->file.bpp = x->file.ch * x->file.bits / 8;
  x->client = x->file;
  *out = x; return 0;
}
static int ios_ExtAudioFileGetProperty(XAF *x, uint32_t pid, uint32_t *size, void *out) {
  if (!x) return -50;
  if (pid == 0x2366726d) { *(int64_t *)out = x->frames; return 0; }                  // '#frm'
  if (pid == 0x66666d74 || pid == 0x63666d74) { memcpy(out, pid == 0x66666d74 ? &x->file : &x->client, sizeof(ASBD)); return 0; }
  dslog("[falta-implementar] ExtAudioFileGetProperty %08x", pid); return -50;
}
static int ios_ExtAudioFileSetProperty(XAF *x, uint32_t pid, uint32_t size, const void *in) {
  if (x && pid == 0x63666d74) { memcpy(&x->client, in, sizeof(ASBD)); return 0; }   // 'cfmt'
  return 0;
}
typedef struct { uint32_t n; struct { uint32_t ch, size; void *data; } b[1]; } ABL;
static int ios_ExtAudioFileRead(XAF *x, uint32_t *frames, ABL *list) {
  if (!x || !list || !list->n) { *frames = 0; return -50; }
  uint32_t want = *frames, cch = x->client.ch ? x->client.ch : x->file.ch, fch = x->file.ch;
  uint32_t room = list->b[0].size / (cch * 2); if (want > room) want = room;
  if (want > x->frames - x->pos) want = x->frames - x->pos;
  int16_t *dst = list->b[0].data; const int16_t *src = (const int16_t *)x->data + x->pos * fch;
  for (uint32_t i = 0; i < want; i++)
    for (uint32_t c = 0; c < cch; c++) dst[i * cch + c] = src[i * fch + (c < fch ? c : 0)];
  x->pos += want; *frames = want; list->b[0].size = want * cch * 2;
  return 0;
}
static int ios_ExtAudioFileDispose(XAF *x) { if (x) { free(x->data); free(x); } return 0; }

uintptr_t audio_resolve_symbol(const char *name) {
  static const struct { const char *n; void *p; } t[] = {
    {"_alcOpenDevice", ios_alcOpenDevice}, {"_alcCloseDevice", ios_alcCloseDevice}, {"_alcCreateContext", ios_alcCreateContext},
    {"_alcMakeContextCurrent", ios_alcMakeContextCurrent}, {"_alcProcessContext", ios_alcVoid}, {"_alcSuspendContext", ios_alcVoid},
    {"_alcDestroyContext", ios_alcVoid}, {"_alcGetError", ios_alcGetError}, {"_alGetError", ios_alGetError},
    {"_alGenBuffers", ios_alGenBuffers}, {"_alDeleteBuffers", ios_alDeleteBuffers}, {"_alIsBuffer", ios_alIsBuffer},
    {"_alBufferData", ios_alBufferData}, {"_alGenSources", ios_alGenSources}, {"_alDeleteSources", ios_alDeleteSources},
    {"_alIsSource", ios_alIsSource}, {"_alSourcei", ios_alSourcei}, {"_alSourcef", ios_alSourcef}, {"_alSource3f", ios_alSource3f},
    {"_alListener3f", ios_alListener3f}, {"_alListenerfv", ios_alListenerfv}, {"_alSourcePlay", ios_alSourcePlay},
    {"_alSourceStop", ios_alSourceStop}, {"_alSourcePause", ios_alSourcePause}, {"_alGetSourcei", ios_alGetSourcei},
    {"_alGetSourcef", ios_alGetSourcef},
    {"_AudioSessionInitialize", ios_AudioSessionZero}, {"_AudioSessionSetProperty", ios_AudioSessionZero},
    {"_AudioSessionSetActive", ios_AudioSessionZero}, {"_AudioSessionGetProperty", ios_AudioSessionGetProperty},
    {"_ExtAudioFileOpenURL", ios_ExtAudioFileOpenURL}, {"_ExtAudioFileGetProperty", ios_ExtAudioFileGetProperty},
    {"_ExtAudioFileSetProperty", ios_ExtAudioFileSetProperty}, {"_ExtAudioFileRead", ios_ExtAudioFileRead},
    {"_ExtAudioFileDispose", ios_ExtAudioFileDispose},
  };
  for (unsigned i = 0; i < sizeof(t) / sizeof(t[0]); i++) if (!strcmp(t[i].n, name)) return (uintptr_t)t[i].p;
  return 0;
}
void audio_init(void) { sceKernelCreateLwMutex(&g_amx, "al", 2 /* recursive */, 0, NULL); }
