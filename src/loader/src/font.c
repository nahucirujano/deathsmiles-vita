// Text engine: stb_truetype with Liberation Sans Bold (Latin, Arial/Helvetica metrics) + IPAGothic (Japanese and fallback symbols).
// Replaces HiraKakuProN-W6 / iOS system fonts. Draws into RGBA8 buffers (not premultiplied).
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <psp2/io/fcntl.h>
#include <psp2/kernel/threadmgr.h>
#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"
#include "ds.h"
#include "font.h"
#include "blend.h"

static stbtt_fontinfo g_lat, g_jp;
static int g_ok_lat, g_ok_jp;
static SceKernelLwMutexWork g_fmx;
static void *load(const char *p) {
  SceUID fd = sceIoOpen(p, SCE_O_RDONLY, 0); if (fd < 0) return NULL;
  int n = sceIoLseek(fd, 0, SCE_SEEK_END); sceIoLseek(fd, 0, SCE_SEEK_SET);
  void *b = malloc(n); sceIoRead(fd, b, n); sceIoClose(fd); return b;
}
static void metrics_init(void);
void font_init(void) {
  sceKernelCreateLwMutex(&g_fmx, "font", 2, 0, NULL);
  void *a = load("app0:res/LiberationSans-Bold.ttf"), *b = load("app0:res/ipag.ttf");
  g_ok_lat = a && stbtt_InitFont(&g_lat, a, 0);
  g_ok_jp = b && stbtt_InitFont(&g_jp, b, 0);
  metrics_init();
  dslog("fonts: Latin %s, Japanese %s", g_ok_lat ? "OK" : "MISSING", g_ok_jp ? "OK" : "MISSING");
}

static uint32_t utf8_next(const char **s) {
  const uint8_t *p = (const uint8_t *)*s; uint32_t c = *p++;
  if (c >= 0xf0) { c = (c & 7) << 18 | (p[0] & 63) << 12 | (p[1] & 63) << 6 | (p[2] & 63); p += 3; }
  else if (c >= 0xe0) { c = (c & 15) << 12 | (p[0] & 63) << 6 | (p[1] & 63); p += 2; }
  else if (c >= 0xc0) { c = (c & 31) << 6 | (p[0] & 63); p += 1; }
  *s = (const char *)p; return c;
}
static stbtt_fontinfo *pick_raw(uint32_t c, int *glyph) {
  if (g_ok_lat && c < 0x2000) { int g = stbtt_FindGlyphIndex(&g_lat, c); if (g) { *glyph = g; return &g_lat; } }
  if (g_ok_jp) { int g = stbtt_FindGlyphIndex(&g_jp, c); if (g || !g_ok_lat) { *glyph = g; return &g_jp; } }
  *glyph = g_ok_lat ? stbtt_FindGlyphIndex(&g_lat, c) : 0; return g_ok_lat ? &g_lat : NULL;
}
// Metrics caches. Previously, for every letter and every measure/draw, the glyph was looked up in the cmap
// table and advance, vertical metrics, units per em and glyph box were read from the TTF (3–5 ms per
// DSUITextLabel text box). Now: code point -> (font, glyph, advance) in a direct table; vertical metrics
// and scale per font computed once; the glyph box is stored with its bitmap.
typedef struct { uint32_t c; stbtt_fontinfo *f; int g, adv; } CP;
#define CP_N 2048
static CP g_cp[CP_N];
static stbtt_fontinfo *pick_adv(uint32_t c, int *glyph, int *advu) {
  CP *e = &g_cp[(c ^ (c >> 11)) & (CP_N - 1)];
  if (e->f && e->c == c) { *glyph = e->g; *advu = e->adv; return e->f; }
  int g; stbtt_fontinfo *f = pick_raw(c, &g); if (!f) return NULL;
  int a, lsb; stbtt_GetGlyphHMetrics(f, g, &a, &lsb);
  e->c = c; e->f = f; e->g = g; e->adv = a; *glyph = g; *advu = a; return f;
}
static float g_em_lat = 1, g_em_jp = 1; static int g_asc_lat, g_desc_lat, g_asc_jp, g_desc_jp;
static void metrics_init(void) {
  int gap;
  if (g_ok_lat) { g_em_lat = stbtt_ScaleForMappingEmToPixels(&g_lat, 1.0f); stbtt_GetFontVMetrics(&g_lat, &g_asc_lat, &g_desc_lat, &gap); }
  if (g_ok_jp) { g_em_jp = stbtt_ScaleForMappingEmToPixels(&g_jp, 1.0f); stbtt_GetFontVMetrics(&g_jp, &g_asc_jp, &g_desc_jp, &gap); }
}
// iOS size in points ≈ height of "M"; stb scales by ascent-descent height
static float scale_for(stbtt_fontinfo *f, float size) { return size * (f == &g_jp ? g_em_jp : g_em_lat); }
static float adv(uint32_t c, float size) {
  int g, a; stbtt_fontinfo *f = pick_adv(c, &g, &a); if (!f) return size * 0.5f;
  return a * scale_for(f, size);
}
float font_line_height(float size) { return ceilf(size * 1.2f); }

// Splits into lines: '\n' forces a break; if maxw > 0, wraps at words (or at characters in CJK).
typedef struct { const char *s, *e; float w; } Line;
static int layout(const char *t, float size, float maxw, int maxlines, Line *out, int cap) {
  int n = 0; const char *p = t;
  while (*p && n < cap) {
    const char *ls = p, *lb = NULL; float w = 0;
    while (*p && *p != '\n') {
      const char *q = p; uint32_t c = utf8_next(&q); float cw = adv(c, size);
      if (maxw > 0 && w + cw > maxw && p != ls) { if (lb && lb > ls) p = lb; break; }
      w += cw; p = q;
      if (c == ' ' || c >= 0x3000) lb = p;      // a line can break after a space or after a CJK character
    }
    const char *le = p;
    while (le > ls && le[-1] == ' ') le--;
    out[n].s = ls; out[n].e = le; out[n].w = 0;
    for (const char *q = ls; q < le;) out[n].w += adv(utf8_next(&q), size);
    n++;
    if (*p == '\n') p++;
    while (*p == ' ' && maxw > 0) p++;
    if (maxlines > 0 && n >= maxlines) break;
  }
  return n;
}
void font_measure(const char *t, float size, float maxw, int maxlines, float *w, float *h) {
  if (!t || !*t) { *w = 0; *h = font_line_height(size); return; }
  Line L[64]; sceKernelLockLwMutex(&g_fmx, 1, NULL);
  int n = layout(t, size, maxw, maxlines, L, 64);
  sceKernelUnlockLwMutex(&g_fmx, 1);
  float mw = 0; for (int i = 0; i < n; i++) if (L[i].w > mw) mw = L[i].w;
  *w = ceilf(mw); *h = n * font_line_height(size);
}

// Rasterized glyph cache (previously every letter was rasterized on every draw): key font+glyph+scale+subpixel.
#define GC_N 2048
static struct { stbtt_fontinfo *f; int g; float sc, sub; int x0, y0, w, h, valid; uint8_t *bm; } g_gc[GC_N];
static unsigned g_gc_hits, g_gc_miss;
// Returns the bitmap (or NULL if the glyph has no pixels) and its box; everything is cached.
static uint8_t *glyph_get(stbtt_fontinfo *f, int g, float sc, float sub, int *x0, int *y0, int *w, int *h) {
  uint32_t k; memcpy(&k, &sc, 4);
  unsigned i = ((unsigned)g * 2654435761u ^ k * 40503u ^ (unsigned)(sub * 4) * 97u ^ (f == &g_jp)) % GC_N;
  if (g_gc[i].valid && g_gc[i].f == f && g_gc[i].g == g && g_gc[i].sc == sc && g_gc[i].sub == sub) {
    g_gc_hits++; *x0 = g_gc[i].x0; *y0 = g_gc[i].y0; *w = g_gc[i].w; *h = g_gc[i].h; return g_gc[i].bm;
  }
  free(g_gc[i].bm); g_gc[i].bm = NULL;
  int bx0, by0, bx1, by1; stbtt_GetGlyphBitmapBoxSubpixel(f, g, sc, sc, sub, 0, &bx0, &by0, &bx1, &by1);
  int gw = bx1 - bx0, gh = by1 - by0;
  uint8_t *bm = NULL;
  if (gw > 0 && gh > 0 && gw < 512 && gh < 512) { bm = malloc(gw * gh); stbtt_MakeGlyphBitmapSubpixel(f, bm, gw, gh, gw, sc, sc, sub, 0, g); }
  g_gc[i].f = f; g_gc[i].g = g; g_gc[i].sc = sc; g_gc[i].sub = sub; g_gc[i].x0 = bx0; g_gc[i].y0 = by0;
  g_gc[i].w = gw; g_gc[i].h = gh; g_gc[i].bm = bm; g_gc[i].valid = 1;
  if ((++g_gc_miss & 1023) == 0) dslog("[OK] glyph cache: %u hits, %u rasterized", g_gc_hits, g_gc_miss);
  *x0 = bx0; *y0 = by0; *w = gw; *h = gh; return bm;
}
// Draws text with the top-left corner of the block at (x, y) of the buffer.
// align: 0 left, 1 centre, 2 right (relative to boxw if > 0). Alpha-blends over the existing content.
void font_draw(uint8_t *buf, int bw, int bh, int pitch, const char *t, float x, float y, float boxw, float size,
               uint32_t rgba, int align, float maxw, int maxlines) {
  if (!t || !*t || (!g_ok_lat && !g_ok_jp)) return;
  int cr = rgba >> 24, cg = (rgba >> 16) & 255, cb = (rgba >> 8) & 255, ca = rgba & 255;
  Line L[64];
  sceKernelLockLwMutex(&g_fmx, 1, NULL);
  int n = layout(t, size, maxw, maxlines, L, 64);
  float lh = font_line_height(size);
  for (int li = 0; li < n; li++) {
    float pen = x;
    if (boxw > 0 && align == 1) pen = x + (boxw - L[li].w) / 2;
    else if (boxw > 0 && align == 2) pen = x + boxw - L[li].w;
    for (const char *q = L[li].s; q < L[li].e;) {
      uint32_t c = utf8_next(&q); int g, advu; stbtt_fontinfo *f = pick_adv(c, &g, &advu);
      if (!f) continue;
      float sc = scale_for(f, size);
      int asc = f == &g_jp ? g_asc_jp : g_asc_lat, desc = f == &g_jp ? g_desc_jp : g_desc_lat;
      float base = y + li * lh + (lh - (asc - desc) * sc) / 2 + asc * sc;
      float sub = floorf((pen - floorf(pen)) * 4) / 4;   // subpixel offset quantized to 1/4 (for the cache)
      int x0, y0, gw, gh; uint8_t *gb = glyph_get(f, g, sc, sub, &x0, &y0, &gw, &gh);
      if (gb) {
        int ox = (int)floorf(pen) + x0, oy = (int)floorf(base) + y0;
        for (int yy = 0; yy < gh; yy++) {
          int py = oy + yy; if (py < 0 || py >= bh) continue;
          uint8_t *row = buf + py * pitch;
          for (int xx = 0; xx < gw; xx++) {
            int px = ox + xx; if (px < 0 || px >= bw) continue;
            int a = gb[yy * gw + xx]; if (!a) continue;
            if (ca < 255) a = mul255(a * ca);
            blend_px(row + px * 4, cr, cg, cb, a);
          }
        }
      }
      pen += advu * sc;
    }
  }
  sceKernelUnlockLwMutex(&g_fmx, 1);
}
