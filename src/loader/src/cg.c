// Minimal CoreGraphics: RGBA8888 bitmap contexts, CGImage decoded with stb_image, UIImage and UIGraphics.
// The game uses it to turn images (PNG) into pixels before uploading them as textures.
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO
#include "stb_image.h"
#include <psp2/io/fcntl.h>
#include <psp2/kernel/processmgr.h>
#include "objc.h"
#include "blend.h"
uint32_t g_recip[256];
void blend_init(void) { g_recip[0] = 0; for (int n = 1; n < 256; n++) g_recip[n] = (uint32_t)(((1ull << 24) + n - 1) / n); }

typedef struct { float x, y, w, h; } CGRect;
typedef struct { float w, h; } CGSize;
typedef struct { float a, b, c, d, tx, ty; } CTM;
typedef struct CGImg { uint32_t magic; int w, h, has_alpha; uint8_t *rgba; int rc; } CGImg;   // non-premultiplied RGBA
struct DL;
typedef struct { uint32_t magic; uint8_t *data; int w, h, bpr, own, info, bpp; uint32_t fill; CTM ctm, stack[16]; int sp; CGRect clip; struct DL *rec; } Ctx;
// ---- Draw list: drawRect: runs while "recording" the operations that paint pixels instead of
// painting them. If the list is identical to the previous drawing of that view, the texture is reused and
// nothing is rasterized; if it changed, the list is replayed. drawRect: runs only ONCE in both cases (no double
// side effects). Reason: the shop (DSUIShopList ~20 ms) and DSUIGuideFrame (~12 ms) ask for a redraw every frame.
enum { OP_TEXT, OP_IMG, OP_CGIMG, OP_CLEAR };
typedef struct { int op; CTM ctm; uint32_t fill; float a[7]; int i[3]; void *obj; char *txt; } DLOp;
typedef struct DL { DLOp *v; int n, cap; uint32_t hash; } DL;
static void dl_mix(DL *d, const void *p, int n) { const uint8_t *b = p; for (int k = 0; k < n; k++) { d->hash ^= b[k]; d->hash *= 16777619u; } }
static DLOp *dl_add(DL *d, int op, Ctx *c) {
  if (d->n == d->cap) { d->cap = d->cap ? d->cap * 2 : 32; d->v = realloc(d->v, d->cap * sizeof(DLOp)); }
  DLOp *o = &d->v[d->n++]; memset(o, 0, sizeof *o); o->op = op; o->ctm = c->ctm; o->fill = c->fill;
  dl_mix(d, &op, sizeof op); dl_mix(d, &c->ctm, sizeof c->ctm); dl_mix(d, &c->fill, sizeof c->fill);
  return o;
}
#define IMG_MAGIC 0x494d4731u
#define CTX_MAGIC 0x43545831u
static int g_cs_dummy;

// ---- CGImage ----
static CGImg *img_new(int w, int h) { CGImg *i = calloc(1, sizeof(CGImg)); i->magic = IMG_MAGIC; i->w = w; i->h = h; i->rgba = calloc(w * h, 4); i->rc = 1; return i; }
static CGImg *img_load(const char *path) {
  SceUID fd = sceIoOpen(path, SCE_O_RDONLY, 0);
  if (fd < 0) return NULL;
  int sz = sceIoLseek(fd, 0, SCE_SEEK_END); sceIoLseek(fd, 0, SCE_SEEK_SET);
  uint64_t t0 = sceKernelGetProcessTimeWide();
  uint8_t *buf = malloc(sz); sceIoRead(fd, buf, sz); sceIoClose(fd);
  uint64_t t1 = sceKernelGetProcessTimeWide();
  int w, h, ch; uint8_t *px = stbi_load_from_memory(buf, sz, &w, &h, &ch, 4); free(buf);
  uint64_t t2 = sceKernelGetProcessTimeWide();
  // Load-time measurement (file read vs. PNG decode with stb_image); logged only in verbose builds
  if (px && w * h >= 256 * 256)
    dslogv("%s image load %s %dx%d (%d KiB file): read %.1f ms, PNG decode %.1f ms%s", (t2 - t1) > 100000 ? "[CPU-bound]" : "[OK]",
          path, w, h, sz / 1024, (t1 - t0) / 1000.0, (t2 - t1) / 1000.0, (t2 - t1) > 100000 ? " [CPU-bound: PNG decode (stb_image inflate) on the loader thread]" : "");
  if (!px) { dslog("[error] could not decode %s", path); return NULL; }
  CGImg *i = calloc(1, sizeof(CGImg)); i->magic = IMG_MAGIC; i->w = w; i->h = h; i->has_alpha = (ch == 4 || ch == 2); i->rgba = px; i->rc = 1;
  return i;
}
static int ios_CGImageGetWidth(CGImg *i) { return i ? i->w : 0; }
static int ios_CGImageGetHeight(CGImg *i) { return i ? i->h : 0; }
static int ios_CGImageGetBitsPerPixel(CGImg *i) { return 32; }
static int ios_CGImageGetAlphaInfo(CGImg *i) { return i && i->has_alpha ? 3 /* kCGImageAlphaLast */ : 5 /* NoneSkipLast */; }
static void *ios_CGImageGetColorSpace(CGImg *i) { return &g_cs_dummy; }
static void ios_CGImageRelease(CGImg *i) { if (i && i->magic == IMG_MAGIC && --i->rc == 0) { free(i->rgba); i->magic = 0; free(i); } }

// ---- colour spaces ----
static void *ios_CGColorSpaceCreateDeviceRGB(void) { return &g_cs_dummy; }
static void ios_CGColorSpaceRelease(void *cs) {}
static int ios_CGColorSpaceGetModel(void *cs) { return 1; }   // kCGColorSpaceModelRGB

// ---- contexto bitmap ----
static const CTM IDENT = {1, 0, 0, 1, 0, 0};
static Ctx *ctx_new(void *data, int w, int h, int bpr, int info, int bpp) {
  Ctx *c = calloc(1, sizeof(Ctx)); c->magic = CTX_MAGIC; c->w = w; c->h = h; c->bpp = bpp; c->bpr = bpr ? bpr : w * bpp; c->info = info;
  c->data = data; if (!data) { c->data = calloc(c->bpr, h); c->own = 1; }
  c->ctm = IDENT; c->clip = (CGRect){0, 0, w, h};
  return c;
}
static Ctx *ios_CGBitmapContextCreate(void *data, uint32_t w, uint32_t h, uint32_t bpc, uint32_t bpr, void *cs, uint32_t info) {
  if (bpc != 8 && bpc != 5) dslog("[not-implemented] CGBitmapContextCreate with %u bits per component", bpc);
  return ctx_new(data, w, h, bpr, info, bpc == 5 ? 2 : 4);   // 5 bpc = 16 bpp xRGB1555
}
static void *ios_CGBitmapContextGetData(Ctx *c) { return c ? c->data : NULL; }
static void ios_CGContextRelease(Ctx *c) { if (c && c->magic == CTX_MAGIC) { if (c->own) free(c->data); c->magic = 0; free(c); } }
SOFTFP static void ios_CGContextTranslateCTM(Ctx *c, float x, float y) { c->ctm.tx += c->ctm.a * x + c->ctm.c * y; c->ctm.ty += c->ctm.b * x + c->ctm.d * y; }
SOFTFP static void ios_CGContextScaleCTM(Ctx *c, float sx, float sy) { c->ctm.a *= sx; c->ctm.b *= sx; c->ctm.c *= sy; c->ctm.d *= sy; }
static void ios_CGContextSaveGState(Ctx *c) { if (c->sp < 16) c->stack[c->sp++] = c->ctm; }
static void ios_CGContextRestoreGState(Ctx *c) { if (c->sp) c->ctm = c->stack[--c->sp]; }
SOFTFP static void ios_CGContextClipToRect(Ctx *c, CGRect r) { c->clip = r; }
static void ios_CGContextSetBlendMode(Ctx *c, int m) {}
SOFTFP static void ios_CGContextClearRect(Ctx *c, CGRect r) {
  if (c->rec) { dl_add(c->rec, OP_CLEAR, c); return; }
  for (int y = 0; y < c->h; y++) memset(c->data + y * c->bpr, 0, c->w * c->bpp);
}
// Draws the image in the rect (CG space: origin bottom-left) transformed by the CTM.
// Translation + scale only (what the game uses). Bitmap memory has row 0 at the top.
SOFTFP static void ios_CGContextDrawImage_raw(Ctx *c, CGRect r, CGImg *img);
SOFTFP static void ios_CGContextDrawImage(Ctx *c, CGRect r, CGImg *img) {
  if (!c || !img || img->magic != IMG_MAGIC) return;
  if (c->rec) {
    DLOp *o = dl_add(c->rec, OP_CGIMG, c); o->a[0] = r.x; o->a[1] = r.y; o->a[2] = r.w; o->a[3] = r.h; o->obj = img; img->rc++;
    dl_mix(c->rec, o->a, 4 * sizeof(float)); dl_mix(c->rec, &img, sizeof img); dl_mix(c->rec, &img->rgba, sizeof img->rgba);
    return;
  }
  uint64_t dt0 = sceKernelGetProcessTimeWide();
  ios_CGContextDrawImage_raw(c, r, img);
  uint64_t dt = sceKernelGetProcessTimeWide() - dt0;
  if (img->w * img->h >= 256 * 256)
    dslogv("%s CGContextDrawImage %dx%d -> %dx%d bitmap (%d bpp): %.1f ms%s", dt > 50000 ? "[CPU-bound]" : "[OK]", img->w, img->h, c->w, c->h, c->bpp * 8,
          dt / 1000.0, dt > 50000 ? " [CPU-bound: per-pixel image copy/convert on the loader thread]" : "");
}
SOFTFP static void ios_CGContextDrawImage_raw(Ctx *c, CGRect r, CGImg *img) {
  float x0 = c->ctm.a * r.x + c->ctm.tx, x1 = c->ctm.a * (r.x + r.w) + c->ctm.tx;
  float y0 = c->ctm.d * r.y + c->ctm.ty, y1 = c->ctm.d * (r.y + r.h) + c->ctm.ty;
  int flipx = x1 < x0, flipy = y1 < y0;
  if (flipx) { float t = x0; x0 = x1; x1 = t; }
  if (flipy) { float t = y0; y0 = y1; y1 = t; }
  int ix0 = (int)floorf(x0), ix1 = (int)ceilf(x1), iy0 = (int)floorf(y0), iy1 = (int)ceilf(y1);
  if (ix0 < 0) ix0 = 0; if (iy0 < 0) iy0 = 0; if (ix1 > c->w) ix1 = c->w; if (iy1 > c->h) iy1 = c->h;
  float dw = x1 - x0, dh = y1 - y0; if (dw <= 0 || dh <= 0) return;
  int premul = (c->info & 0x1f) == 1 || (c->info & 0x1f) == 2;   // kCGImageAlphaPremultipliedLast/First
  static int sxs[2048]; if (ix1 - ix0 > 2048) ix1 = ix0 + 2048;
  for (int x = ix0; x < ix1; x++) {   // precomputed source column (previously: a float division per pixel)
    float u = (x + 0.5f - x0) / dw;
    int sx = (int)((flipx ? 1.0f - u : u) * img->w); if (sx < 0) sx = 0; if (sx >= img->w) sx = img->w - 1;
    sxs[x - ix0] = sx;
  }
  for (int y = iy0; y < iy1; y++) {
    float v = (y + 0.5f - y0) / dh;                  // 0 bottom .. 1 top in CG space
    int sy = (int)((flipy ? v : 1.0f - v) * img->h); if (sy < 0) sy = 0; if (sy >= img->h) sy = img->h - 1;
    uint8_t *dst = c->data + (c->h - 1 - y) * c->bpr;
    for (int x = ix0; x < ix1; x++) {
      const uint8_t *s = img->rgba + (sy * img->w + sxs[x - ix0]) * 4;
      if (c->bpp == 2) {   // xRGB1555 (16 bits, order according to kCGBitmapByteOrder16Big/Little)
        uint16_t v = ((s[0] >> 3) << 10) | ((s[1] >> 3) << 5) | (s[2] >> 3);
        if ((c->info & 0x7000) == 0x3000) v = (v >> 8) | (v << 8);
        ((uint16_t *)dst)[x] = v; continue;
      }
      uint8_t *d = dst + x * 4;
      int a = img->has_alpha ? s[3] : 255;
      int sr = s[0], sg = s[1], sb = s[2];
      if (premul && a != 255) { sr = mul255(sr * a); sg = mul255(sg * a); sb = mul255(sb * a); }
      if (a == 255 || !premul) { d[0] = sr; d[1] = sg; d[2] = sb; d[3] = a; }
      else { int ia = 255 - a; d[0] = sr + mul255(d[0] * ia); d[1] = sg + mul255(d[1] * ia); d[2] = sb + mul255(d[2] * ia); d[3] = a + mul255(d[3] * ia); }
    }
  }
}
static CGImg *ios_CGBitmapContextCreateImage(Ctx *c) {
  CGImg *i = img_new(c->w, c->h); i->has_alpha = c->bpp == 4;
  for (int y = 0; y < c->h; y++) {
    if (c->bpp == 4) { memcpy(i->rgba + y * c->w * 4, c->data + y * c->bpr, c->w * 4); continue; }
    for (int x = 0; x < c->w; x++) {
      uint16_t v = ((uint16_t *)(c->data + y * c->bpr))[x]; uint8_t *o = i->rgba + (y * c->w + x) * 4;
      o[0] = ((v >> 10) & 31) * 255 / 31; o[1] = ((v >> 5) & 31) * 255 / 31; o[2] = (v & 31) * 255 / 31; o[3] = 255;
    }
  }
  return i;
}

// ---- UIImage ----
typedef struct { Class_t *isa; CGImg *img; } UIImg;
static Class_t *C_UIImage;
static id uiimage_from(CGImg *i) { if (!i) return NULL; UIImg *u = (UIImg *)objc_alloc(C_UIImage, 0); u->img = i; return (id)u; }
static const char *app_path(const char *name, char *buf, int n) {
  if (!strncmp(name, "ux0:", 4)) return name;
  snprintf(buf, n, "ux0:data/deathsmiles/app/%s", name); return buf;
}
// imageNamed: as on iOS, with a cache (previously the PNG was read and decoded on every call: the shop list
// asks for its icons on every redraw -> disk reads + stb_image every frame, hitches and music cuts).
// 48 MiB limit; the oldest entries are discarded.
#define IC_N 256
#define IC_MAX_BYTES (48u << 20)
static struct { char name[96]; CGImg *img; uint32_t bytes; } g_ic[IC_N];
static int g_ic_n; static uint32_t g_ic_bytes; static unsigned g_ic_hits, g_ic_loads;
static void ic_drop_oldest(void) {
  if (!g_ic_n) return;
  g_ic_bytes -= g_ic[0].bytes; ios_CGImageRelease(g_ic[0].img);
  memmove(&g_ic[0], &g_ic[1], (g_ic_n - 1) * sizeof(g_ic[0])); g_ic_n--;
}
IMPL static id UIImg_named(Class_t *c, SEL s, id name) {
  const char *nm = nsstring_utf8(name); if (!nm) return NULL;
  for (int k = g_ic_n - 1; k >= 0; k--) if (!strcmp(g_ic[k].name, nm)) {
    g_ic_hits++; CGImg *i = g_ic[k].img; i->rc++; return objc_autorelease_(uiimage_from(i));
  }
  char b[512]; const char *p = app_path(nm, b, sizeof b);
  uint64_t t0 = sceKernelGetProcessTimeWide();
  CGImg *i = img_load(p);
  if (!i) { dslog("[warning] UIImage: %s not found", p); return NULL; }
  unsigned ms = (unsigned)((sceKernelGetProcessTimeWide() - t0) / 1000);
  if (++g_ic_loads <= 200 && ms >= 10) dslogv("[CPU-bound] imageNamed %s: %u ms (PNG read+decode, now cached)", nm, ms);
  if (strlen(nm) < sizeof(g_ic[0].name)) {
    uint32_t bytes = (uint32_t)i->w * i->h * 4;
    while (g_ic_n && (g_ic_n >= IC_N || g_ic_bytes + bytes > IC_MAX_BYTES)) ic_drop_oldest();
    strcpy(g_ic[g_ic_n].name, nm); g_ic[g_ic_n].img = i; g_ic[g_ic_n].bytes = bytes; g_ic_n++; g_ic_bytes += bytes;
    i->rc++;   // cache reference
  }
  if ((g_ic_hits + g_ic_loads) % 2000 == 0) dslogv("[OK] image cache: %u hits, %u loads, %u KiB", g_ic_hits, g_ic_loads, g_ic_bytes >> 10);
  return objc_autorelease_(uiimage_from(i));
}
IMPL static id UIImg_initFile(UIImg *u, SEL s, id path) {
  char b[512]; CGImg *i = img_load(app_path(nsstring_utf8(path), b, sizeof b));
  if (!i) { dslog("[warning] UIImage initWithContentsOfFile: %s not found", nsstring_utf8(path)); objc_free_object((id)u); return NULL; }
  u->img = i; return (id)u;
}
IMPL static id UIImg_initCG(UIImg *u, SEL s, CGImg *i) { if (i) i->rc++; u->img = i; return (id)u; }
IMPL static id UIImg_withCG(Class_t *c, SEL s, CGImg *i) { if (i) i->rc++; return objc_autorelease_(uiimage_from(i)); }
IMPL static CGSize UIImg_size(UIImg *u, SEL s) { CGSize z = {u->img ? u->img->w : 0, u->img ? u->img->h : 0}; return z; }
IMPL static CGImg *UIImg_CGImage(UIImg *u, SEL s) { return u->img; }
IMPL static float UIImg_scale(UIImg *u, SEL s) { return 1.0f; }
IMPL static void UIImg_dealloc(UIImg *u, SEL s) { ios_CGImageRelease(u->img); objc_free_object((id)u); }

// ---- UIGraphics (image context stack) ----
static Ctx *g_gstack[8]; static int g_gsp;
SOFTFP static void ios_UIGraphicsBeginImageContext(CGSize sz) {
  Ctx *c = ctx_new(NULL, (int)sz.w, (int)sz.h, 0, 1, 4);
  c->ctm = (CTM){1, 0, 0, -1, 0, sz.h};     // UIKit: origin top-left
  if (g_gsp < 8) g_gstack[g_gsp++] = c;
}
static Ctx *ios_UIGraphicsGetCurrentContext(void) { return g_gsp ? g_gstack[g_gsp - 1] : NULL; }
static id ios_UIGraphicsGetImageFromCurrentImageContext(void) { Ctx *c = ios_UIGraphicsGetCurrentContext(); return c ? objc_autorelease_(uiimage_from(ios_CGBitmapContextCreateImage(c))) : NULL; }
static void ios_UIGraphicsEndImageContext(void) { if (g_gsp) ios_CGContextRelease(g_gstack[--g_gsp]); }

// ---- internal API for UIKit (text and images in the current context, UIKit coordinates) ----
#include "font.h"
void *cg_current(void) { return g_gsp ? g_gstack[g_gsp - 1] : NULL; }
void cg_push(void *c) { if (g_gsp < 8) g_gstack[g_gsp++] = c; }
void cg_pop(void) { if (g_gsp) g_gsp--; }
void *cg_new_uikit(int w, int h) { Ctx *c = ctx_new(NULL, w, h, 0, 1, 4); c->ctm = (CTM){1, 0, 0, -1, 0, h}; c->fill = 0x000000ff; return c; }
uint8_t *cg_data(void *ctx, int *w, int *h, int *pitch) { Ctx *c = ctx; *w = c->w; *h = c->h; *pitch = c->bpr; return c->data; }
void cg_free(void *ctx) { ios_CGContextRelease(ctx); }
void cg_set_fill(void *ctx, uint32_t rgba) { if (ctx) ((Ctx *)ctx)->fill = rgba; }
uint32_t cg_get_fill(void *ctx) { return ctx ? ((Ctx *)ctx)->fill : 0x000000ff; }
// UIKit point -> (column, row) of the buffer
static void ui_to_dev(Ctx *c, float x, float y, float *dx, float *dy) {
  float cx = c->ctm.a * x + c->ctm.c * y + c->ctm.tx, cy = c->ctm.b * x + c->ctm.d * y + c->ctm.ty;
  *dx = cx; *dy = c->h - cy;
}
void cg_text(void *ctx, const char *t, float x, float y, float boxw, float size, int align, float maxw, int maxlines) {
  Ctx *c = ctx; if (!c || c->bpp != 4) return;
  if (c->rec) {
    DLOp *o = dl_add(c->rec, OP_TEXT, c);
    o->a[0] = x; o->a[1] = y; o->a[2] = boxw; o->a[3] = size; o->a[4] = maxw; o->i[0] = align; o->i[1] = maxlines;
    o->txt = strdup(t ? t : ""); dl_mix(c->rec, o->a, 5 * sizeof(float)); dl_mix(c->rec, o->i, 2 * sizeof(int)); dl_mix(c->rec, o->txt, strlen(o->txt));
    return;
  }
  float dx, dy; ui_to_dev(c, x, y, &dx, &dy);
  font_draw(c->data, c->w, c->h, c->bpr, t, dx, dy, boxw, size, c->fill, align, maxw, maxlines);
}
void cg_image(void *ctx, void *uiimage, float x, float y, float w, float h, float alpha) {
  Ctx *c = ctx; UIImg *u = uiimage;
  if (!c || !u || !u->img || c->bpp != 4) return;
  CGImg *img = u->img;
  if (c->rec) {
    DLOp *o = dl_add(c->rec, OP_IMG, c);
    o->a[0] = x; o->a[1] = y; o->a[2] = w; o->a[3] = h; o->a[4] = alpha; o->obj = objc_retain_((id)u);
    dl_mix(c->rec, o->a, 5 * sizeof(float)); dl_mix(c->rec, &img, sizeof img);
    dl_mix(c->rec, &img->rgba, sizeof img->rgba); dl_mix(c->rec, &img->w, 2 * sizeof(int));
    return;
  }
  if (w <= 0) w = img->w; if (h <= 0) h = img->h;
  float dx, dy; ui_to_dev(c, x, y, &dx, &dy);
  int ix0 = (int)floorf(dx), iy0 = (int)floorf(dy), iw = (int)(w + 0.5f), ih = (int)(h + 0.5f);
  int ga = (int)(alpha * 255);
  if (iw <= 0 || ih <= 0) return;
  static int sxs[2048]; if (iw > 2048) iw = 2048;
  for (int xx = 0; xx < iw; xx++) sxs[xx] = xx * img->w / iw;      // source columns, once per image
  for (int yy = 0; yy < ih; yy++) {
    int py = iy0 + yy; if (py < 0 || py >= c->h) continue;
    int sy = yy * img->h / ih; uint8_t *row = c->data + py * c->bpr; const uint8_t *srow = img->rgba + sy * img->w * 4;
    for (int xx = 0; xx < iw; xx++) {
      int px = ix0 + xx; if (px < 0 || px >= c->w) continue;
      const uint8_t *s = srow + sxs[xx] * 4;
      int a = img->has_alpha ? s[3] : 255; if (ga < 255) a = mul255(a * ga);
      blend_px(row + px * 4, s[0], s[1], s[2], a);
    }
  }
}
// Recording: cg_rec_begin(ctx) before drawRect:, cg_rec_end(ctx) returns the signature; then cg_rec_play (draws)
// or cg_rec_drop (discards). Always one of the two.
void cg_rec_begin(void *ctx) { Ctx *c = ctx; c->rec = calloc(1, sizeof(DL)); c->rec->hash = 2166136261u; }
uint32_t cg_rec_end(void *ctx) { Ctx *c = ctx; return c->rec ? c->rec->hash ^ (uint32_t)c->rec->n : 0; }
static void dl_free(DL *d) {
  for (int k = 0; k < d->n; k++) {
    DLOp *o = &d->v[k]; free(o->txt);
    if (o->op == OP_IMG) objc_release_((id)o->obj); else if (o->op == OP_CGIMG) ios_CGImageRelease(o->obj);
  }
  free(d->v); free(d);
}
void cg_rec_drop(void *ctx) { Ctx *c = ctx; if (c->rec) { dl_free(c->rec); c->rec = NULL; } }
void cg_rec_play(void *ctx) {
  Ctx *c = ctx; DL *d = c->rec; if (!d) return;
  c->rec = NULL; CTM ctm = c->ctm; uint32_t fill = c->fill;
  for (int k = 0; k < d->n; k++) {
    DLOp *o = &d->v[k]; c->ctm = o->ctm; c->fill = o->fill;
    if (o->op == OP_TEXT) cg_text(c, o->txt, o->a[0], o->a[1], o->a[2], o->a[3], o->i[0], o->a[4], o->i[1]);
    else if (o->op == OP_IMG) cg_image(c, o->obj, o->a[0], o->a[1], o->a[2], o->a[3], o->a[4]);
    else if (o->op == OP_CGIMG) ios_CGContextDrawImage(c, (CGRect){o->a[0], o->a[1], o->a[2], o->a[3]}, o->obj);
    else ios_CGContextClearRect(c, (CGRect){0, 0, 0, 0});
  }
  c->ctm = ctm; c->fill = fill; dl_free(d);
}
int cg_uiimage_size(void *uiimage, int *w, int *h) { UIImg *u = uiimage; if (!u || !u->img) return 0; *w = u->img->w; *h = u->img->h; return 1; }

uintptr_t cg_resolve_symbol(const char *name) {
  static const struct { const char *n; void *p; } t[] = {
    {"_CGImageGetWidth", ios_CGImageGetWidth}, {"_CGImageGetHeight", ios_CGImageGetHeight},
    {"_CGImageGetBitsPerPixel", ios_CGImageGetBitsPerPixel}, {"_CGImageGetAlphaInfo", ios_CGImageGetAlphaInfo},
    {"_CGImageGetColorSpace", ios_CGImageGetColorSpace}, {"_CGImageRelease", ios_CGImageRelease},
    {"_CGColorSpaceCreateDeviceRGB", ios_CGColorSpaceCreateDeviceRGB}, {"_CGColorSpaceRelease", ios_CGColorSpaceRelease},
    {"_CGColorSpaceGetModel", ios_CGColorSpaceGetModel},
    {"_CGBitmapContextCreate", ios_CGBitmapContextCreate}, {"_CGBitmapContextGetData", ios_CGBitmapContextGetData},
    {"_CGBitmapContextCreateImage", ios_CGBitmapContextCreateImage}, {"_CGContextRelease", ios_CGContextRelease},
    {"_CGContextTranslateCTM", ios_CGContextTranslateCTM}, {"_CGContextScaleCTM", ios_CGContextScaleCTM},
    {"_CGContextSaveGState", ios_CGContextSaveGState}, {"_CGContextRestoreGState", ios_CGContextRestoreGState},
    {"_CGContextClipToRect", ios_CGContextClipToRect}, {"_CGContextSetBlendMode", ios_CGContextSetBlendMode},
    {"_CGContextClearRect", ios_CGContextClearRect}, {"_CGContextDrawImage", ios_CGContextDrawImage},
    {"_UIGraphicsBeginImageContext", ios_UIGraphicsBeginImageContext}, {"_UIGraphicsGetCurrentContext", ios_UIGraphicsGetCurrentContext},
    {"_UIGraphicsGetImageFromCurrentImageContext", ios_UIGraphicsGetImageFromCurrentImageContext},
    {"_UIGraphicsEndImageContext", ios_UIGraphicsEndImageContext},
  };
  for (unsigned i = 0; i < sizeof(t) / sizeof(t[0]); i++) if (!strcmp(t[i].n, name)) return (uintptr_t)t[i].p;
  return 0;
}
void cg_init(void) {
  blend_init();
  C_UIImage = objc_getClass("UIImage");
  class_add_meta(C_UIImage, "imageNamed:", UIImg_named); class_add_meta(C_UIImage, "imageWithContentsOfFile:", UIImg_named);
  class_add_meta(C_UIImage, "imageWithCGImage:", UIImg_withCG);
  class_add(C_UIImage, "initWithContentsOfFile:", UIImg_initFile); class_add(C_UIImage, "initWithCGImage:", UIImg_initCG);
  class_add(C_UIImage, "size", UIImg_size); class_add(C_UIImage, "CGImage", UIImg_CGImage);
  class_add(C_UIImage, "scale", UIImg_scale); class_add(C_UIImage, "dealloc", UIImg_dealloc);
}
