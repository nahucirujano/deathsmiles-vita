// OpenGL ES 1.1 (iOS) -> vitaGL bridge.
// The game's "screen" framebuffer (the one EAGL attaches to the layer) is redirected to vitaGL's
// framebuffer 0, and viewport/scissor are scaled to centre the game image on 960x544.
#include <vitaGL.h>
#include <string.h>
#include <math.h>
#include <stdlib.h>
#include <math.h>
#include <stdio.h>
#include <psp2/kernel/sysmem.h>
#include "ds.h"
#include "gfx.h"

static int g_gl_ready;
struct VP { GLint sz; GLenum ty; GLsizei st; const void *p; }; static struct VP g_vp_[3];   // the game's vertex, color, texcoord
static GLuint g_fbB;   // FBO where the game composes the 480x320 screen
static float g_pf_x0 = 27, g_pf_x1 = 453;
static float g_skip_r[4], g_skip_dx; static unsigned g_skip_frame, g_frame_no = 100;
// tap on the moved SKIP button -> original button position (480x320 game coordinates)
// ---- CPU at 444 MHz only while loading ----
// Stage loads spend 0.5–0.8 s per texture in the game's own loader (measured).
// 444 MHz is an official Vita clock speed (no plugins). It is raised when the main thread stops producing
// frames (>60 ms) or when a large texture is uploaded, and goes back to the base speed 0.5 s later.
// If the base speed is already >=444 (user overclock), nothing is changed.
#include <psp2/power.h>
static volatile uint64_t g_boost_until;
void gfx_boost_hint(void) { g_boost_until = sceKernelGetProcessTimeWide() + 500000; }
static int clock_thread(SceSize n, void *p) {
  int base = scePowerGetArmClockFrequency();
  dslog("CPU clock at start: %d MHz%s", base, base >= 444 ? " (>=444: load boost disabled)" : "");
  if (base >= 444) return sceKernelExitDeleteThread(0);
  unsigned last = g_frame_no; uint64_t last_t = sceKernelGetProcessTimeWide(), b0 = 0; int boosted = 0;
  for (;;) {
    sceKernelDelayThread(20000);
    uint64_t now = sceKernelGetProcessTimeWide();
    if (g_frame_no != last) { last = g_frame_no; last_t = now; }
    else if (now - last_t > 60000 && now - last_t < 6000000) g_boost_until = now + 500000;   // main thread stalled (loading)
    int want = now < g_boost_until;
    if (want != boosted) {
      int r = scePowerSetArmClockFrequency(want ? 444 : base); boosted = want;
      if (want) b0 = now;
      else dslogv("[CPU-bound] load boost: CPU at 444 MHz for %.0f ms, back to %d MHz (%08x) [CPU-bound: game decompressor on the loader thread]",
                 (now - b0) / 1000.0, base, r);
    }
  }
  return 0;
}
void gfx_boost_start(void) {
  SceUID t = sceKernelCreateThread("ds_clock", clock_thread, 0x10000100 - 0x20, 0x4000, 0, 0x40000 /* core 2 */, NULL);
  if (t >= 0) sceKernelStartThread(t, 0, NULL);
}
void gfx_skip_remap(float *x, float *y) {
  if (g_frame_no - g_skip_frame > 10) return;
  if (*x >= g_skip_r[0] - 6 && *x <= g_skip_r[2] + 6 && *y >= g_skip_r[1] - 6 && *y <= g_skip_r[3] + 6) *x += g_skip_dx;
}   // playfield inside the 480x320 composed screen
#define TEXNAMES 512
static char g_tex_file[TEXNAMES][16]; static uint16_t g_tex_w[TEXNAMES], g_tex_h[TEXNAMES];   // texture -> source data*.bin
static int g_draw_dump_quads;
extern int ui_touch_mode(void);
static int ui_touch_mode_c(void) { return ui_touch_mode(); }
int g_draw_dump;   // diagnostics: 2 = requested, 1 = dumping this frame
static GLenum g_mm = GL_MODELVIEW; static float g_col[4] = {1, 1, 1, 1}; static GLenum g_bs = GL_ONE, g_bd = GL_ZERO;
int g_gfx_active;
static void apply_vp_sc(void);
#include "tstate.h"
static TState *vpts(void) {
  TState *t = ts_get();
  if (!t->vp_init) { t->vp_init = 1; t->vp[2] = t->sc[2] = 480; t->vp[3] = t->sc[3] = 320; }
  return t;
}
#define g_vp (vpts()->vp)
#define g_sc (vpts()->sc)

#include <psp2/kernel/threadmgr.h>
#include "tstate.h"
// vitaGL is not thread-safe: all the game's GL calls go through a recursive mutex.
// On iOS each game thread uses its own EAGLContext; we emulate the minimum of that state
// (bound texture and framebuffer) and restore it when the thread using GL changes.
static SceKernelLwMutexWork g_glmx; static int g_glmx_ok; static SceUID g_gl_owner;
static void gl_bind_fb_raw(GLuint fb);
static uint64_t g_main_wait, g_main_wait_max;   // µs the main thread waited for the GL lock
void gfx_lock_report(void) {
  if (g_main_wait > 50000)
    dslogv("[CPU-bound] main thread waited %.0f ms for the GL lock in 5 s (longest %.0f ms) [CPU-bound: texture uploads on the loader thread block the main thread]",
          g_main_wait / 1000.0, g_main_wait_max / 1000.0);
  g_main_wait = g_main_wait_max = 0;
}
static void gl_enter(void) {
  if (!g_glmx_ok) { sceKernelCreateLwMutex(&g_glmx, "gl", 2, 0, NULL); g_glmx_ok = 1; }
  // Measures how long the main thread waits for the GL lock (texture uploads on another thread).
  extern int threads_is_main(void);
  if (threads_is_main() && sceKernelTryLockLwMutex(&g_glmx, 1) < 0) {
    uint64_t t0 = sceKernelGetProcessTimeWide();
    sceKernelLockLwMutex(&g_glmx, 1, NULL);
    uint64_t w = sceKernelGetProcessTimeWide() - t0; g_main_wait += w; if (w > g_main_wait_max) g_main_wait_max = w;
  } else if (!threads_is_main()) sceKernelLockLwMutex(&g_glmx, 1, NULL);
  SceUID me = sceKernelGetThreadId();
  if (me != g_gl_owner) {
    TState *t = ts_get();
    if (g_gl_owner) { glBindTexture(GL_TEXTURE_2D, t->tex); gl_bind_fb_raw(t->fb); }
    g_gl_owner = me;
  }
}
static void gl_leave(void) { sceKernelUnlockLwMutex(&g_glmx, 1); }
static GLuint g_cur_fb, g_cur_rb;
static GLuint g_screen_rb;               // renderbuffer attached to the screen
static int g_game_w = 480, g_game_h = 320;
static float g_sx = 2.0f, g_sy = 2.0f; static int g_ox, g_oy;
#define MAXFB 64
static struct { GLuint fb, color_rb; } g_fbs[MAXFB];
#define MAXRB 64
static struct { GLuint id; int w, h; } g_rbs[MAXRB];
static GLuint g_next_rb = 0x1000;

static GLboolean splash_none(const vglSplashDetails *d);
void gfx_init(void) {
  if (g_gl_ready) return;
  // The "#c76f…" number at the bottom left was the vitaGL commit (c76f75e) that its splash screen
  // draws next to the spinning logo. It is painted black on the logo's black background.
  vglSetCustomSplashscreenHashColor(0.0f, 0.0f, 0.0f, 0.0f);
  // No spinning vitaGL logo; a black screen while loading. vitaGL's own splash screen API is used
  // with empty callbacks: init does not load the logo's 3D model and main only clears the screen
  // to black and reports it is done, so it ends as soon as the game draws its first frame.
  vglSetupCustomSplashscreen(splash_none, NULL, splash_none, GL_TRUE);
  vglInitExtended(0, 960, 544, 0x1000000, SCE_GXM_MULTISAMPLE_NONE);
  g_gl_ready = 1; g_gfx_active = 1;
  { void gfx_boost_start(void); gfx_boost_start(); }
  SceKernelFreeMemorySizeInfo mi = {sizeof(mi)};
  sceKernelGetFreeMemorySize(&mi);
  dslog("[GPU-bound] vitaGL started 960x544. Free: RAM %u MiB, CDRAM(VMEM) %u MiB, phycont %u MiB",
        mi.size_user >> 20, mi.size_cdram >> 20, mi.size_phycont >> 20);
}
static int fb_is_screen(GLuint fb) {
  if (fb == 0) return 1;
  for (int i = 0; i < MAXFB; i++) if (g_fbs[i].fb == fb) return g_screen_rb && g_fbs[i].color_rb == g_screen_rb;
  return 0;
}
static int screen_bound(void) { return fb_is_screen(g_cur_fb); }

void gfx_set_screen(int w, int h) {
  int changed = g_screen_rb != g_cur_rb || g_game_w != w || g_game_h != h;
  g_screen_rb = g_cur_rb; g_game_w = w; g_game_h = h;
  float s = 960.0f / w; if (544.0f / h < s) s = 544.0f / h;
  g_sx = g_sy = s; g_ox = (int)((960 - w * s) / 2); g_oy = (int)((544 - h * s) / 2);
  for (int i = 0; i < MAXRB; i++) if (g_rbs[i].id == g_cur_rb) { g_rbs[i].w = w; g_rbs[i].h = h; }
  if (changed) dslogv("[GPU-bound] game screen %dx%d -> %dx%d at (%d,%d) scale x1000=%d", w, h, (int)(w * s), (int)(h * s), g_ox, g_oy, (int)(s * 1000));
  if (screen_bound()) { glBindFramebuffer(GL_FRAMEBUFFER, 0); apply_vp_sc(); }
}
float gfx_scale(void) { return g_sx; }
int gfx_ox(void) { return g_ox; }
int gfx_oy(void) { return g_oy; }
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WRITE_NO_STDIO
#include "stb_image_write.h"
#include <psp2/io/fcntl.h>
#include "font.h"
#include <psp2/io/stat.h>
static void shot_write(void *ctx, void *data, int size) { sceIoWrite(*(SceUID *)ctx, data, size); }
static void take_screenshot(void) {
  static int n;
  uint8_t *px = malloc(960 * 544 * 4);
  if (!px) return;
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  glReadPixels(0, 0, 960, 544, GL_RGBA, GL_UNSIGNED_BYTE, px);
  for (int y = 0; y < 272; y++) for (int x = 0; x < 960 * 4; x++) { uint8_t t = px[y * 3840 + x]; px[y * 3840 + x] = px[(543 - y) * 3840 + x]; px[(543 - y) * 3840 + x] = t; }
  for (int i = 0; i < 960 * 544; i++) px[i * 4 + 3] = 255;
  char path[96]; sceIoMkdir("ux0:data/deathsmiles/shots", 0777);
  snprintf(path, sizeof path, "ux0:data/deathsmiles/shots/shot_%03d.png", n++);
  SceUID fd = sceIoOpen(path, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
  if (fd >= 0) { stbi_write_png_to_func(shot_write, &fd, 960, 544, 4, px, 960 * 4); sceIoClose(fd); dslog("captura guardada: %s", path); }
  free(px);
  gl_bind_fb_raw(g_cur_fb);
}
// ---- composition of UIKit views (text, windows) on top of the game frame ----
void gfx_upload_view(uint32_t *tex, const uint8_t *rgba, int w, int h) {
  if (!*tex) glGenTextures(1, tex);
  glBindTexture(GL_TEXTURE_2D, *tex);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
}
static int g_composing;
static void compose_begin(void) {
  if (!g_composing) {
    g_composing = 1;
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, 960, 544); glDisable(GL_ALPHA_TEST);
    // clip to the visible game area: text/cursor must not spill into the side strips
    int sx0 = g_ox < 0 ? 0 : g_ox, sy0 = g_oy < 0 ? 0 : g_oy;
    int sx1 = g_ox + (int)(g_game_w * g_sx), sy1 = g_oy + (int)(g_game_h * g_sy);
    if (g_game_w <= 0) { sx1 = 960; sy1 = 544; }
    if (sx1 > 960) sx1 = 960; if (sy1 > 544) sy1 = 544;
    glEnable(GL_SCISSOR_TEST); glScissor(sx0, 544 - sy1, sx1 - sx0, sy1 - sy0);
    glMatrixMode(GL_PROJECTION); glPushMatrix(); glLoadIdentity(); glOrthof(0, 960, 544, 0, -1, 1);
    glMatrixMode(GL_MODELVIEW); glPushMatrix(); glLoadIdentity();
    glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA); glEnable(GL_TEXTURE_2D);
    glEnableClientState(GL_VERTEX_ARRAY); glEnableClientState(GL_TEXTURE_COORD_ARRAY); glDisableClientState(GL_COLOR_ARRAY);
  }
}
void gfx_draw_view_tex(uint32_t tex, int w, int h, float x, float y, float alpha) {
  compose_begin();
  float X = g_ox + x * g_sx, Y = 544 - (g_oy + (y + h) * g_sy), W = w * g_sx, H = h * g_sy;   // game y downwards -> screen
  Y = g_oy + y * g_sy;
  float v[8] = {X, Y, X + W, Y, X, Y + H, X + W, Y + H}, t[8] = {0, 0, 1, 0, 0, 1, 1, 1};
  glBindTexture(GL_TEXTURE_2D, tex); glColor4f(1, 1, 1, alpha);
  glVertexPointer(2, GL_FLOAT, 0, v); glTexCoordPointer(2, GL_FLOAT, 0, t);
  glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}
static void compose_end(GLboolean blend, GLboolean tex2d, GLboolean scis, GLboolean alpha, GLboolean va, GLboolean ca, GLboolean ta) {
  if (!g_composing) return;
  g_composing = 0;
  glMatrixMode(GL_PROJECTION); glPopMatrix(); glMatrixMode(GL_MODELVIEW); glPopMatrix(); glMatrixMode(g_mm);
  if (!blend) glDisable(GL_BLEND); glBlendFunc(g_bs, g_bd);
  if (!tex2d) glDisable(GL_TEXTURE_2D); if (!scis) glDisable(GL_SCISSOR_TEST); if (alpha) glEnable(GL_ALPHA_TEST);
  if (!va) glDisableClientState(GL_VERTEX_ARRAY); if (!ta) glDisableClientState(GL_TEXTURE_COORD_ARRAY); if (ca) glEnableClientState(GL_COLOR_ARRAY);
  if (g_vp_[0].p) glVertexPointer(g_vp_[0].sz, g_vp_[0].ty, g_vp_[0].st, g_vp_[0].p);
  if (g_vp_[1].p) glColorPointer(g_vp_[1].sz, g_vp_[1].ty, g_vp_[1].st, g_vp_[1].p);
  if (g_vp_[2].p) glTexCoordPointer(g_vp_[2].sz, g_vp_[2].ty, g_vp_[2].st, g_vp_[2].p);
  glColor4f(g_col[0], g_col[1], g_col[2], g_col[3]);
  glBindTexture(GL_TEXTURE_2D, ts_get()->tex);
  gl_bind_fb_raw(g_cur_fb);
}
void ui_compose(void);
// ---- Bezel: 960x544 PNG images. During gameplay the game's own frame is removed
// (see w_glDrawArrays) and the two side strips of the image (118 px each) are drawn beside
// the playfield (x 118–842). The centre of the image is not used.
#define BEZEL_W 118
extern unsigned char *stbi_load_from_memory(const unsigned char *b, int len, int *x, int *y, int *comp, int req);
// If the image is not in ux0:data/deathsmiles/, the copy included in the VPK (app0:bezels/) is used.
// So the bezels come with the installation and can be replaced by placing a PNG in ux0:data/deathsmiles/.
static GLboolean splash_none(const vglSplashDetails *d) { (void)d; return GL_TRUE; }
static unsigned char *load_png(const char *path, int *w, int *h) {
  SceUID fd = sceIoOpen(path, SCE_O_RDONLY, 0);
  if (fd < 0 && !strncmp(path, "ux0:data/deathsmiles/", 21)) {
    char alt[160]; snprintf(alt, sizeof alt, "app0:bezels/%s", path + 21); fd = sceIoOpen(alt, SCE_O_RDONLY, 0);
    if (fd >= 0) dslogv("bezel: using the VPK copy %s", alt);
  }
  if (fd < 0) return NULL;
  int n = sceIoLseek(fd, 0, SCE_SEEK_END); sceIoLseek(fd, 0, SCE_SEEK_SET);
  unsigned char *buf = malloc(n); sceIoRead(fd, buf, n); sceIoClose(fd);
  int ch; unsigned char *px = stbi_load_from_memory(buf, n, w, h, &ch, 4); free(buf); return px;
}
extern void stbi_image_free(void *p);
static GLuint g_bezel_tex; static int g_bezel_state, g_bezel_w, g_bezel_h;   // state: 0 not tried, 1 loaded, -1 missing
static int g_cursor_vis;   // menu cursor visible (pause, Continue): do not hide small pieces (Yes/No, Resume…)
static int g_play_frame;   // in this frame the game composed the gameplay screen
static int g_since_play = 1000;   // frames since the last gameplay screen
int gfx_in_gameplay(void) { return g_since_play < 30; }   // gameplay (includes pause and Continue, which draw the game behind them)
// Per-character bezel: the game stores the current character in a global integer (game 0x3439dc, written at
// game+0x61eb2 next to the "*** CharaNo : %d" log). Order of the names in the executable: WINDIA, CASPER,
// FOLLETT, ROSA (0..3). Files: bezel_<name>.png;
// if missing, bezel.png. The textures are kept (up to 5 × ~2 MiB of VRAM).
extern uintptr_t g_base; extern uint32_t g_vmbase;
// 4 = unused slot; 5 = Tiara (Smartphone mode)
static const char *g_chara_names[6] = {"windia", "casper", "follett", "rosa", "personaje4", "tiara"};
#define NCHARA 6
static GLuint g_btex[NCHARA + 1]; static int g_bstate[NCHARA + 1];   // 0 not tried, 1 loaded, -1 missing; last = bezel.png
static int g_cur_chara = -2;
static GLuint bezel_tex_for(int k) {
  if (!g_bstate[k]) {
    char path[96];
    if (k < NCHARA) snprintf(path, sizeof path, "ux0:data/deathsmiles/bezel_%s.png", g_chara_names[k]);
    else snprintf(path, sizeof path, "ux0:data/deathsmiles/bezel.png");
    int w, h; unsigned char *px = load_png(path, &w, &h);
    if (!px) { g_bstate[k] = -1; dslogv("bezel: %s not found", path); }
    else {
      glGenTextures(1, &g_btex[k]); glBindTexture(GL_TEXTURE_2D, g_btex[k]);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
      glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
      free(px); g_bstate[k] = 1;
      dslogv("[VMEM-bound] bezel loaded: %s %dx%d (%d KiB of VRAM)", path, w, h, w * h * 4 / 1024);
    }
  }
  return g_bstate[k] == 1 ? g_btex[k] : 0;
}
static void bezel_load(void) {   // picks the bezel for the current character (checked on every gameplay frame)
  int c = *(volatile int32_t *)(g_base + 0x3439dc - g_vmbase);
  if (c == g_cur_chara && g_bezel_state) return;
  g_cur_chara = c;
  GLuint t = (c >= 0 && c < NCHARA) ? bezel_tex_for(c) : 0;
  if (!t) t = bezel_tex_for(NCHARA);
  g_bezel_tex = t; g_bezel_state = t ? 1 : -1;
  dslogv("bezel: personaje %d (%s) -> %s", c, (c >= 0 && c < NCHARA) ? g_chara_names[c] : "?", t ? "character bezel" : "game's own frame");
}
// Outside gameplay the game only draws its central rectangle and never clears the sides of the screen:
// without this the last bezel image stayed "stuck" in menus and loading screens. The side strips are painted black.
static int g_sides_clean;
static void sides_clear(void) {
  if (g_sides_clean >= 3) return;   // 3 frames are enough (vitaGL uses up to 3 screen buffers)
  g_sides_clean++;
  int xl = g_ox > 0 ? g_ox : 0, xr = g_ox + (int)(g_game_w * g_sx); if (xr > 960) xr = 960;
  compose_begin(); glDisable(GL_SCISSOR_TEST); glDisable(GL_TEXTURE_2D); glDisable(GL_BLEND);
  glDisableClientState(GL_TEXTURE_COORD_ARRAY); glColor4f(0, 0, 0, 1);
  float vL[8] = {0, 0, xl, 0, 0, 544, xl, 544}, vR[8] = {xr, 0, 960, 0, xr, 544, 960, 544};
  if (xl > 0) { glVertexPointer(2, GL_FLOAT, 0, vL); glDrawArrays(GL_TRIANGLE_STRIP, 0, 4); }
  if (xr < 960) { glVertexPointer(2, GL_FLOAT, 0, vR); glDrawArrays(GL_TRIANGLE_STRIP, 0, 4); }
  glEnableClientState(GL_TEXTURE_COORD_ARRAY); glEnable(GL_TEXTURE_2D); glEnable(GL_BLEND); glEnable(GL_SCISSOR_TEST);
}
// Menu bezel (bezel_menu.png, 960x544): outside gameplay the game uses x 72–888, leaving 72 px per side.
static GLuint g_menu_tex; static int g_menu_state;
static void menu_bezel_draw(void) {
  if (!g_menu_state) {
    int w, h; unsigned char *px = load_png("ux0:data/deathsmiles/bezel_menu.png", &w, &h);
    if (!px) { g_menu_state = -1; dslogv("bezel: ux0:data/deathsmiles/bezel_menu.png not found (menus with black strips)"); }
    else {
      glGenTextures(1, &g_menu_tex); glBindTexture(GL_TEXTURE_2D, g_menu_tex);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
      glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, px); free(px); g_menu_state = 1;
      dslogv("[VMEM-bound] menu bezel loaded: %dx%d (%d KiB of VRAM)", w, h, w * h * 4 / 1024);
    }
  }
  int xl = g_ox > 0 ? g_ox : 0, xr = g_ox + (int)(g_game_w * g_sx); if (xr > 960) xr = 960;
  if (g_menu_state != 1) { if (g_bezel_state == 1) sides_clear(); return; }
  compose_begin(); glDisable(GL_SCISSOR_TEST); glDisable(GL_BLEND);
  glBindTexture(GL_TEXTURE_2D, g_menu_tex); glColor4f(1, 1, 1, 1);
  float uL = xl / 960.0f, uR = xr / 960.0f;
  float vL[8] = {0, 0, xl, 0, 0, 544, xl, 544}, tL[8] = {0, 0, uL, 0, 0, 1, uL, 1};
  float vR[8] = {xr, 0, 960, 0, xr, 544, 960, 544}, tR[8] = {uR, 0, 1, 0, uR, 1, 1, 1};
  if (xl > 0) { glVertexPointer(2, GL_FLOAT, 0, vL); glTexCoordPointer(2, GL_FLOAT, 0, tL); glDrawArrays(GL_TRIANGLE_STRIP, 0, 4); }
  if (xr < 960) { glVertexPointer(2, GL_FLOAT, 0, vR); glTexCoordPointer(2, GL_FLOAT, 0, tR); glDrawArrays(GL_TRIANGLE_STRIP, 0, 4); }
  glEnable(GL_BLEND); glEnable(GL_SCISSOR_TEST);
}
static void bezel_draw(void) {
  extern int ui_talk_active(void);
  static int talk_was;
  int talk = g_play_frame && ui_talk_active();
  if (talk != talk_was) { talk_was = talk; dslogv("bezel: %s", talk ? "dialogue scene -> menu bezel + original frame" : "back to character bezel"); }
  if (talk) { menu_bezel_draw(); return; }
  if (!g_play_frame) { g_sides_clean = (g_menu_state == 1) ? g_sides_clean : g_sides_clean; menu_bezel_draw(); return; }
  if (g_bezel_state != 1 || !g_play_frame) return;
  g_sides_clean = 0;
  compose_begin(); glDisable(GL_SCISSOR_TEST);
  float uL = (float)BEZEL_W / 960.0f, uR = (960.0f - BEZEL_W) / 960.0f;   // proportional: works even if the image is not 960 wide
  float vL[8] = {0, 0, BEZEL_W, 0, 0, 544, BEZEL_W, 544}, tL[8] = {0, 0, uL, 0, 0, 1, uL, 1};
  float vR[8] = {960 - BEZEL_W, 0, 960, 0, 960 - BEZEL_W, 544, 960, 544}, tR[8] = {uR, 0, 1, 0, uR, 1, 1, 1};
  glBindTexture(GL_TEXTURE_2D, g_bezel_tex); glColor4f(1, 1, 1, 1); glDisable(GL_BLEND);
  glVertexPointer(2, GL_FLOAT, 0, vL); glTexCoordPointer(2, GL_FLOAT, 0, tL); glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
  glVertexPointer(2, GL_FLOAT, 0, vR); glTexCoordPointer(2, GL_FLOAT, 0, tR); glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
  glEnable(GL_BLEND); glEnable(GL_SCISSOR_TEST);
}
// ---- Short on-screen notice (control type change) ----
// gfx_toast can be called from any thread: it only copies the text; the texture is built in compose_frame (main thread).
#define TOAST_W 320
#define TOAST_H 40
static char g_toast_txt[64]; static volatile int g_toast_new; static int g_toast_frames; static GLuint g_toast_tex;
void gfx_toast(const char *t) { snprintf(g_toast_txt, sizeof g_toast_txt, "%s", t); g_toast_new = 1; }
static void toast_draw(void) {
  if (g_toast_new) {
    g_toast_new = 0;
    uint8_t *px = malloc(TOAST_W * TOAST_H * 4);
    if (px) {
      for (int i = 0; i < TOAST_W * TOAST_H; i++) { px[i*4] = px[i*4+1] = px[i*4+2] = 0; px[i*4+3] = 190; }
      font_draw(px, TOAST_W, TOAST_H, TOAST_W * 4, g_toast_txt, 0, (TOAST_H - font_line_height(22)) / 2, TOAST_W, 22,
                0xFFFFFFFF, 1, TOAST_W, 1);
      if (!g_toast_tex) glGenTextures(1, &g_toast_tex);
      glBindTexture(GL_TEXTURE_2D, g_toast_tex);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
      glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, TOAST_W, TOAST_H, 0, GL_RGBA, GL_UNSIGNED_BYTE, px); free(px);
      g_toast_frames = 120;   // ~2 s a 60 FPS
    }
  }
  if (g_toast_frames <= 0) return;
  g_toast_frames--;
  float a = g_toast_frames < 30 ? g_toast_frames / 30.0f : 1.0f;
  float x0 = (960 - TOAST_W) / 2, y0 = 24;
  float v[8] = {x0, y0, x0 + TOAST_W, y0, x0, y0 + TOAST_H, x0 + TOAST_W, y0 + TOAST_H}, t[8] = {0, 0, 1, 0, 0, 1, 1, 1};
  compose_begin(); glDisable(GL_SCISSOR_TEST); glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
  glBindTexture(GL_TEXTURE_2D, g_toast_tex); glColor4f(1, 1, 1, a);
  glVertexPointer(2, GL_FLOAT, 0, v); glTexCoordPointer(2, GL_FLOAT, 0, t); glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
  glColor4f(1, 1, 1, 1); glEnable(GL_SCISSOR_TEST);
}
static void compose_frame(void) {
  GLboolean blend = glIsEnabled(GL_BLEND), tex2d = glIsEnabled(GL_TEXTURE_2D), scis = glIsEnabled(GL_SCISSOR_TEST),
            alpha = glIsEnabled(GL_ALPHA_TEST), va = glIsEnabled(GL_VERTEX_ARRAY), ca = glIsEnabled(GL_COLOR_ARRAY),
            ta = glIsEnabled(GL_TEXTURE_COORD_ARRAY);
  uint64_t t0 = sceKernelGetProcessTimeWide();
  if (g_play_frame) bezel_load(); else if (!g_bezel_state) bezel_load();
  { extern int ui_cursor_visible(void); g_cursor_vis = ui_cursor_visible(); }
  bezel_draw(); g_frame_no++; g_play_frame = 0; if (g_since_play < 1000) g_since_play++;
  ui_compose();
  toast_draw();
  compose_end(blend, tex2d, scis, alpha, va, ca, ta);
  static uint64_t acc, n; acc += sceKernelGetProcessTimeWide() - t0;
  if (++n == 300) { void ui_render_report(void); ui_render_report(); dslogv("%s UIKit composition: %.2f ms per frame", acc / n > 2000 ? "[CPU-bound]" : "[OK]", acc / 1000.0 / n); acc = n = 0; }
}
static uint32_t g_frames, g_t0;
int input_take_shot_request(void);
void gfx_present(void) {
  gl_enter();
  extern int threads_is_main(void);
  if (threads_is_main()) compose_frame();
  if (input_take_shot_request()) take_screenshot();
  vglSwapBuffers(GL_FALSE); gl_leave();
  if (g_draw_dump == 1) { g_draw_dump = 0; g_draw_dump_quads = 0; dslogv("draw: end of dump"); }
  else if (g_draw_dump == 2) { g_draw_dump = 1; dslogv("draw: dumping one frame"); }
  g_frames++;
  uint32_t now = sceKernelGetProcessTimeLow();
  if (!g_t0) g_t0 = now;
  if (now - g_t0 >= 5000000) {
    float fps = g_frames * 1e6f / (now - g_t0);
    if (DS_VERBOSE || fps < 58.0f) dslog("[GPU-bound] %.1f FPS (5 s average)", fps);
    g_frames = 0; g_t0 = now;
  }
}

// ---- framebuffers / renderbuffers OES ----
static void ios_glGenFramebuffersOES(GLsizei n, GLuint *ids) { gl_enter(); glGenFramebuffers(n, ids); gl_leave(); }
static void ios_glDeleteFramebuffersOES(GLsizei n, const GLuint *ids) {
  for (int k = 0; k < n; k++) for (int i = 0; i < MAXFB; i++) if (g_fbs[i].fb == ids[k]) g_fbs[i].fb = 0;
  glDeleteFramebuffers(n, ids);
}
static void gl_bind_fb_raw(GLuint fb) { g_cur_fb = fb; glBindFramebuffer(GL_FRAMEBUFFER, fb_is_screen(fb) ? 0 : fb); apply_vp_sc(); }
static void ios_glBindFramebufferOES(GLenum t, GLuint fb) { gl_enter(); ts_get()->fb = fb; gl_bind_fb_raw(fb); gl_leave(); }
static void ios_glGenRenderbuffersOES(GLsizei n, GLuint *ids) {
  for (int k = 0; k < n; k++) {
    ids[k] = g_next_rb++;
    for (int i = 0; i < MAXRB; i++) if (!g_rbs[i].id) { g_rbs[i].id = ids[k]; break; }
  }
}
static void ios_glBindRenderbufferOES(GLenum t, GLuint rb) { g_cur_rb = rb; ts_get()->rb = rb; }
static void ios_glRenderbufferStorageOES(GLenum t, GLenum fmt, GLsizei w, GLsizei h) {
  for (int i = 0; i < MAXRB; i++) if (g_rbs[i].id == g_cur_rb) { g_rbs[i].w = w; g_rbs[i].h = h; }
  dslogv("[VMEM-bound] glRenderbufferStorageOES fmt=%#x %dx%d (ignorado)", fmt, w, h);
}
static void ios_glFramebufferRenderbufferOES(GLenum t, GLenum att, GLenum rbt, GLuint rb) {
  if (att != 0x8CE0) return;   // only COLOR_ATTACHMENT0 matters
  for (int i = 0; i < MAXFB; i++) if (g_fbs[i].fb == g_cur_fb) { g_fbs[i].color_rb = rb; return; }
  for (int i = 0; i < MAXFB; i++) if (!g_fbs[i].fb) { g_fbs[i].fb = g_cur_fb; g_fbs[i].color_rb = rb; return; }
}
static GLuint g_rt_tex[8]; static int g_rt_n;   // textures used as render targets (playfield FBO "A")
static int is_rt_tex(GLuint t) { for (int i = 0; i < g_rt_n; i++) if (g_rt_tex[i] == t) return 1; return 0; }
static void ios_glFramebufferTexture2DOES(GLenum t, GLenum att, GLenum tt, GLuint tex, GLint lvl) {
  if (screen_bound()) { dslog("[error] render-to-texture on the screen framebuffer"); return; }
  if (tex && !is_rt_tex(tex) && g_rt_n < 8) g_rt_tex[g_rt_n++] = tex;
  gl_enter(); glFramebufferTexture2D(GL_FRAMEBUFFER, att, tt, tex, lvl); gl_leave();
}
static GLenum ios_glCheckFramebufferStatusOES(GLenum t) { return screen_bound() ? GL_FRAMEBUFFER_COMPLETE : glCheckFramebufferStatus(GL_FRAMEBUFFER); }
static void ios_glGetRenderbufferParameterivOES(GLenum t, GLenum p, GLint *v) {
  for (int i = 0; i < MAXRB; i++) if (g_rbs[i].id == g_cur_rb) {
    if (p == 0x8D42) { *v = g_rbs[i].w; return; }      // WIDTH
    if (p == 0x8D43) { *v = g_rbs[i].h; return; }      // HEIGHT
  }
  *v = 0;
}
static void ios_glGetIntegerv(GLenum p, GLint *v) {
  if (p == 0x8CA6) { *v = g_cur_fb; return; }           // FRAMEBUFFER_BINDING_OES
  if (p == 0x0BA2) { memcpy(v, g_vp, 16); return; }     // GL_VIEWPORT (game values)
  if (p == 0x0C10) { memcpy(v, g_sc, 16); return; }     // GL_SCISSOR_BOX
  if (p == 0x8CA7) { *v = g_cur_rb; return; }           // RENDERBUFFER_BINDING_OES
  glGetIntegerv(p, v);
}

// ---- viewport / scissor scaled when the target is the screen ----
// In GL, viewport and scissor are global state, not per framebuffer. The game sets the viewport
// BEFORE binding its intermediate FBO, so the "game" values are stored and translated
// every time the target changes (screen -> scaled and centred; FBO -> as is).
static void apply_vp_sc(void) {
  if (screen_bound()) {
    glViewport(g_ox + (int)(g_vp[0] * g_sx), g_oy + (int)(g_vp[1] * g_sy), (int)(g_vp[2] * g_sx + 0.5f), (int)(g_vp[3] * g_sy + 0.5f));
    glScissor(g_ox + (int)(g_sc[0] * g_sx), g_oy + (int)(g_sc[1] * g_sy), (int)(g_sc[2] * g_sx + 0.5f), (int)(g_sc[3] * g_sy + 0.5f));
  } else {
    glViewport(g_vp[0], g_vp[1], g_vp[2], g_vp[3]);
    glScissor(g_sc[0], g_sc[1], g_sc[2], g_sc[3]);
  }
}
static void ios_glViewport(GLint x, GLint y, GLsizei w, GLsizei h) {
  gl_enter(); g_vp[0] = x; g_vp[1] = y; g_vp[2] = w; g_vp[3] = h; apply_vp_sc(); gl_leave();
}
static void ios_glScissor(GLint x, GLint y, GLsizei w, GLsizei h) {
  gl_enter(); g_sc[0] = x; g_sc[1] = y; g_sc[2] = w; g_sc[3] = h; apply_vp_sc(); gl_leave();
}

// ---- functions with float: iOS convention (floats in integer registers) ----
SOFTFP static void ios_glClearColor(float r, float g, float b, float a) { gl_enter(); glClearColor(r, g, b, a); gl_leave(); }
SOFTFP static void ios_glColor4f(float r, float g, float b, float a) { gl_enter(); g_col[0] = r; g_col[1] = g; g_col[2] = b; g_col[3] = a; glColor4f(r, g, b, a); gl_leave(); }
SOFTFP static void ios_glOrthof(float l, float r, float b, float t, float n, float f) { gl_enter(); glOrthof(l, r, b, t, n, f); gl_leave(); }
SOFTFP static void ios_glTranslatef(float x, float y, float z) { gl_enter(); glTranslatef(x, y, z); gl_leave(); }
SOFTFP static void ios_glTexParameterf(GLenum t, GLenum p, float v) { gl_enter(); glTexParameterf(t, p, v); gl_leave(); }
SOFTFP static void ios_glAlphaFunc(GLenum f, float r) { gl_enter(); glAlphaFunc(f, r); gl_leave(); }

#define WV(name, params, args) static void w_##name params { gl_enter(); name args; gl_leave(); }
#define WR(ret, name, params, args) static ret w_##name params { gl_enter(); ret r = name args; gl_leave(); return r; }
static void w_glBindTexture(GLenum t, GLuint tex) { gl_enter(); if (t == GL_TEXTURE_2D) ts_get()->tex = tex; glBindTexture(t, tex); gl_leave(); }
WV(glBindBuffer, (GLenum a, GLuint b), (a, b))
static void w_glBlendFunc(GLenum a, GLenum b) { gl_enter(); g_bs = a; g_bd = b; glBlendFunc(a, b); gl_leave(); }
WV(glClear, (GLbitfield a), (a))
static void w_glColorPointer(GLint a, GLenum b, GLsizei c, const void *d) { gl_enter(); g_vp_[1].sz = a; g_vp_[1].ty = b; g_vp_[1].st = c; g_vp_[1].p = d; glColorPointer(a, b, c, d); gl_leave(); }
static uint32_t g_tex_bytes, g_tex_count;
static void tex_log(const char *what, GLenum fmt, GLenum type, int w, int h, uint32_t bytes) {
  GLenum e = glGetError();
  g_tex_count++; g_tex_bytes += bytes;
  TState *t = ts_get();
  if (e || g_tex_count <= 400)
    dslogv("%s%s tex=%u %dx%d fmt=%#x type=%#x thread=%#x err=%#x | total %u textures, %u KiB (free VRAM %u KiB)",
          e ? "[VMEM-bound] [error] " : "", what, t->tex, w, h, fmt, type, t->tid, e, g_tex_count, g_tex_bytes >> 10,
          (unsigned)(vglMemFree(VGL_MEM_VRAM) >> 10));
}
static void w_glCompressedTexImage2D(GLenum a, GLint b, GLenum c, GLsizei d, GLsizei e, GLint f, GLsizei g, const void *h) {
  gl_enter(); glCompressedTexImage2D(a, b, c, d, e, f, g, h); if (!b) tex_log("glCompressedTexImage2D", c, 0, d, e, g); gl_leave();
}
static void w_glDeleteTextures(GLsizei a, const GLuint *b) {
  gl_enter(); if (a > 0) dslogv("glDeleteTextures %d (first=%u) thread=%#x", a, b[0], ts_get()->tid); glDeleteTextures(a, b); gl_leave();
}
WV(glDisable, (GLenum a), (a))
WV(glDisableClientState, (GLenum a), (a))
// ---- dump of one frame's draw calls (diagnostics: SELECT+TRIANGLE, verbose builds) to locate touch buttons and frame pieces ----

static float vget(const struct VP *v, int i, int k) {
  const uint8_t *b = (const uint8_t *)v->p; int st = v->st ? v->st : v->sz * (v->ty == GL_FLOAT ? 4 : v->ty == GL_SHORT ? 2 : 1);
  const uint8_t *e = b + i * st;
  if (v->ty == GL_FLOAT) return ((const float *)e)[k];
  if (v->ty == GL_SHORT) return ((const int16_t *)e)[k];
  if (v->ty == GL_FIXED) return ((const int32_t *)e)[k] / 65536.0f;
  return ((const uint8_t *)e)[k];
}
static void w_glDrawArrays(GLenum a, GLint b, GLsizei c) {
  gl_enter();
  if (g_draw_dump == 1 && g_vp_[0].p) {
    float x0 = 1e9, y0 = 1e9, x1 = -1e9, y1 = -1e9, u0 = 1e9, v0 = 1e9, u1 = -1e9, v1 = -1e9;
    for (int i = b; i < b + c && i < b + 4096; i++) {
      float x = vget(&g_vp_[0], i, 0), y = vget(&g_vp_[0], i, 1);
      if (x < x0) x0 = x; if (x > x1) x1 = x; if (y < y0) y0 = y; if (y > y1) y1 = y;
      if (g_vp_[2].p) { float u = vget(&g_vp_[2], i, 0), v = vget(&g_vp_[2], i, 1); if (u < u0) u0 = u; if (u > u1) u1 = u; if (v < v0) v0 = v; if (v > v1) v1 = v; }
    }
    uint32_t tx = ts_get()->tex;
    dslogv("draw: mode=%u n=%d tex=%u (%s %ux%u) fb=%u xy=(%.0f,%.0f)-(%.0f,%.0f) uv=(%.3f,%.3f)-(%.3f,%.3f)",
          a, c, tx, tx < TEXNAMES ? g_tex_file[tx] : "?", tx < TEXNAMES ? g_tex_w[tx] : 0, tx < TEXNAMES ? g_tex_h[tx] : 0,
          g_cur_fb, x0, y0, x1, y1, u0, v0, u1, v1);
    // each piece (quad) separately, to rebuild illustrations made of several pieces
    int step = a == GL_TRIANGLES ? 6 : 4;
    if ((a == GL_TRIANGLES && c % 6 == 0) || (a == GL_TRIANGLE_STRIP && c == 4)) {
      static int nq; if (g_draw_dump_quads == 0) nq = 0;
      for (int q = b; q + step <= b + c && nq < 4000; q += step, nq++) {
        float qx0 = 1e9, qy0 = 1e9, qx1 = -1e9, qy1 = -1e9, qu0 = 1e9, qv0 = 1e9, qu1 = -1e9, qv1 = -1e9;
        for (int i = q; i < q + step; i++) {
          float x = vget(&g_vp_[0], i, 0), y = vget(&g_vp_[0], i, 1);
          if (x < qx0) qx0 = x; if (x > qx1) qx1 = x; if (y < qy0) qy0 = y; if (y > qy1) qy1 = y;
          if (g_vp_[2].p) { float u = vget(&g_vp_[2], i, 0), v = vget(&g_vp_[2], i, 1); if (u < qu0) qu0 = u; if (u > qu1) qu1 = u; if (v < qv0) qv0 = v; if (v > qv1) qv1 = v; }
        }
        // orientation: u of the first and second vertex (to detect rotated or mirrored pieces)
        float ua = g_vp_[2].p ? vget(&g_vp_[2], q, 0) : 0, va = g_vp_[2].p ? vget(&g_vp_[2], q, 1) : 0;
        float xa = vget(&g_vp_[0], q, 0), ya = vget(&g_vp_[0], q, 1);
        dslogv("  pieza tex=%u xy=(%.1f,%.1f)-(%.1f,%.1f) uv=(%.4f,%.4f)-(%.4f,%.4f) v0=(%.1f,%.1f|%.4f,%.4f)",
              tx, qx0, qy0, qx1, qy1, qu0, qv0, qu1, qv1, xa, ya, ua, va);
      }
      g_draw_dump_quads = 1;
    }
  }
  // On-screen touch controls: the game composes the 480x320 screen in its own FBO
  // (fbB): 1) the 4:3 playfield, a quad (27,0)-(453,320); 2) side frame pieces; 3) touch buttons
  // (POWER UP/LOCK 64x64 at (315,255), and a group with II pause, LASER, LEFT/RIGHT SHOT); 4) a full-screen
  // flash. While the controller is in use, the small quads (<=80x80 = buttons) of that FBO are skipped.
  if (a == GL_TRIANGLE_STRIP && c == 4 && g_vp_[0].p && g_cur_fb == g_fbB && g_fbB && g_play_frame && !ui_touch_mode_c() && !g_cursor_vis) {
    // button glow drawn as a 4-vertex strip: hidden if it is over a known button
    float x0 = 1e9, y0 = 1e9, x1 = -1e9, y1 = -1e9;
    for (int i = b; i < b + 4; i++) { float x = vget(&g_vp_[0], i, 0), y = vget(&g_vp_[0], i, 1); if (x < x0) x0 = x; if (x > x1) x1 = x; if (y < y0) y0 = y; if (y > y1) y1 = y; }
    float cx = (x0 + x1) / 2, cy = (y0 + y1) / 2;
    static const float br[4][4] = {{312, 252, 382, 322}, {0, 0, 60, 40}, {420, 188, 476, 244}, {394, 260, 480, 316}};
    for (int k = 0; k < 4; k++) if (cx >= br[k][0] && cx <= br[k][2] && cy >= br[k][1] && cy <= br[k][3] && x1 - x0 <= 160 && y1 - y0 <= 160) { gl_leave(); return; }
  }
  if (a == GL_TRIANGLES && g_vp_[0].p && c >= 6 && c % 6 == 0 && c <= 96) {
    if (c == 6) {   // is this the playfield quad? -> this FBO is the composed screen
      float x0 = vget(&g_vp_[0], b, 0), y0 = vget(&g_vp_[0], b, 1), x1 = x0, y1 = y0;
      for (int i = b + 1; i < b + 6; i++) { float x = vget(&g_vp_[0], i, 0), y = vget(&g_vp_[0], i, 1); if (x < x0) x0 = x; if (x > x1) x1 = x; if (y < y0) y0 = y; if (y > y1) y1 = y; }
      // Besides the known quad (27,0)-(453,320), any large centred quad that draws the playfield FBO
      // texture off screen. After a save data reset the game's screen options went back to their defaults and
      // the playfield was no longer at (27..453): without this, gameplay was not detected (touch controls
      // visible, pause/quit with buttons broken).
      int known = fabsf(x0 - 27) < 2 && fabsf(x1 - 453) < 2 && fabsf(y0) < 2 && fabsf(y1 - 320) < 2;
      int generic = !known && !screen_bound() && is_rt_tex(ts_get()->tex) && fabsf(x0 + x1 - 480) < 6 && x1 - x0 >= 200 && y1 - y0 >= 200;
      if (is_rt_tex(ts_get()->tex)) {   // diagnostics: where the playfield texture is drawn (first few times)
        static int dn; if (dn < 6 && !known) { dn++; dslogv("diag: quad with FBO texture (tex %u) in fb %u%s: (%.0f,%.0f)-(%.0f,%.0f)",
          ts_get()->tex, g_cur_fb, screen_bound() ? " (screen)" : "", x0, y0, x1, y1); }
      }
      if (known || generic) {
        if (g_fbB != g_cur_fb) { g_fbB = g_cur_fb; dslogv("game composed screen: fb %u", g_fbB); }
        if (fabsf(x0 - g_pf_x0) > 1 || fabsf(x1 - g_pf_x1) > 1) {
          dslogv("[GPU-bound] playfield in the composed screen: (%.0f,%.0f)-(%.0f,%.0f)%s", x0, y0, x1, y1, known ? "" : " (different screen mode)");
          g_pf_x0 = x0; g_pf_x1 = x1;
        }
        g_play_frame = 1; g_since_play = 0;
      }
    }
    extern int ui_touch_mode(void); extern int ui_talk_active(void);
    int hide_buttons = !ui_touch_mode() && !g_cursor_vis, hide_frame = g_bezel_state == 1 && !ui_talk_active();
    // only in gameplay frames: in menus the same FBO composes screens with small pieces (logo, Back…)
    if (g_cur_fb == g_fbB && g_fbB && g_play_frame && (hide_buttons || hide_frame)) {
      int run = -1;
      for (int q = b; q < b + c; q += 6) {
        float x0 = 1e9, y0 = 1e9, x1 = -1e9, y1 = -1e9;
        for (int i = q; i < q + 6; i++) { float x = vget(&g_vp_[0], i, 0), y = vget(&g_vp_[0], i, 1); if (x < x0) x0 = x; if (x > x1) x1 = x; if (y < y0) y0 = y; if (y > y1) y1 = y; }
        int frame = x1 <= g_pf_x0 + 3 || x0 >= g_pf_x1 - 3;   // side frame piece (outside the playfield), 27x240 and 27x80 pieces
        // button: a small piece, or a piece (e.g. the press glow) centred over a known button
        static const float br[4][4] = {{312, 252, 382, 322}, {0, 0, 60, 40}, {420, 188, 476, 244}, {394, 260, 480, 316}};
        float cx = (x0 + x1) / 2, cy = (y0 + y1) / 2; int on_btn = 0;
        for (int k = 0; k < 4; k++) if (cx >= br[k][0] && cx <= br[k][2] && cy >= br[k][1] && cy <= br[k][3]) on_btn = 1;
        int button = !frame && (((x1 - x0) <= 80 && (y1 - y0) <= 80) || (on_btn && (x1 - x0) <= 160 && (y1 - y0) <= 160));
        int skip = (button && hide_buttons) || (frame && hide_frame);
        // SKIP button of cutscenes and credits (top right, peeks out under the character bezel).
        // With the bezel on, it is drawn shifted left, inside the playfield; input.c shifts the tap the same way.
        if (!skip && hide_frame && !frame && x0 >= 400 && x1 > g_pf_x1 - 2 && y1 <= 40 && (x1 - x0) <= 80) {
          float dx = x1 - (g_pf_x1 - 2);
          if (run >= 0) { glDrawArrays(a, run, q - run); run = -1; }
          GLenum mm = g_mm; glMatrixMode(GL_MODELVIEW); glPushMatrix();
          glTranslatef(-dx, 0, 0); glDrawArrays(a, q, 6); glPopMatrix(); glMatrixMode(mm);
          g_skip_r[0] = x0 - dx; g_skip_r[1] = y0; g_skip_r[2] = x1 - dx; g_skip_r[3] = y1; g_skip_dx = dx; g_skip_frame = g_frame_no;
          static int slog; if (slog < 4) { slog++; dslogv("skip button moved %.0f px left: (%.0f,%.0f)-(%.0f,%.0f)", dx, x0, y0, x1, y1); }
          continue;
        }
        static int logged; if (logged < 16) { logged++; dslogv("quad in the composed screen (%.0f,%.0f)-(%.0f,%.0f): %s", x0, y0, x1, y1, button ? "button" : frame ? "adorno lateral" : "otro"); }
        if (skip) { if (run >= 0) { glDrawArrays(a, run, q - run); run = -1; } }
        else if (run < 0) run = q;
      }
      if (run >= 0) glDrawArrays(a, run, b + c - run);
      gl_leave(); return;
    }
  }
  glDrawArrays(a, b, c); gl_leave();
}
WV(glEnable, (GLenum a), (a))
WV(glEnableClientState, (GLenum a), (a))
WV(glFlush, (void), ())
WV(glGenTextures, (GLsizei a, GLuint *b), (a, b))
WR(GLenum, glGetError, (void), ())
WV(glLoadIdentity, (void), ())
static void w_glMatrixMode(GLenum a) { gl_enter(); g_mm = a; glMatrixMode(a); gl_leave(); }
static void w_glTexCoordPointer(GLint a, GLenum b, GLsizei c, const void *d) { gl_enter(); g_vp_[2].sz = a; g_vp_[2].ty = b; g_vp_[2].st = c; g_vp_[2].p = d; glTexCoordPointer(a, b, c, d); gl_leave(); }
static void w_glTexImage2D(GLenum a, GLint b, GLint c, GLsizei d, GLsizei e, GLint f, GLenum g, GLenum h, const void *i) {
  gl_enter();
  static uint64_t last_end; uint64_t t0 = sceKernelGetProcessTimeWide();
  glTexImage2D(a, b, c, d, e, f, g, h, i);
  uint64_t t1 = sceKernelGetProcessTimeWide();
  if (!b && d * e >= 512 * 512) gfx_boost_hint();
  if (!b && d * e >= 256 * 256) {
    double up = (t1 - t0) / 1000.0, gap = last_end && t0 - last_end < 5000000 ? (t0 - last_end) / 1000.0 : -1;
    dslogv("%s texture upload %dx%d type=%#x: %.1f ms inside vitaGL (GL lock held); %.0f ms since previous upload (game reading/decompressing)%s",
          up > 30 ? "[CPU-bound]" : "[OK]", d, e, h, up, gap, up > 30 ? " [CPU-bound: texture format conversion on CPU]" : "");
  }
  last_end = t1;
  if (!b) { TState *ts = ts_get(); uint32_t id = ts->tex;
    if (id < TEXNAMES) { memcpy(g_tex_file[id], ts->last_file, 16); g_tex_w[id] = d; g_tex_h[id] = e; } }
  if (!b) tex_log("glTexImage2D", g, h, d, e, d * e * (h == GL_UNSIGNED_BYTE ? 4 : 2)); gl_leave();
}
WV(glTexParameteri, (GLenum a, GLenum b, GLint c), (a, b, c))
static void w_glVertexPointer(GLint a, GLenum b, GLsizei c, const void *d) { gl_enter(); g_vp_[0].sz = a; g_vp_[0].ty = b; g_vp_[0].st = c; g_vp_[0].p = d; glVertexPointer(a, b, c, d); gl_leave(); }
static const struct { const char *n; void *p; } g_gl[] = {
  {"_glGenFramebuffersOES", ios_glGenFramebuffersOES}, {"_glDeleteFramebuffersOES", ios_glDeleteFramebuffersOES},
  {"_glBindFramebufferOES", ios_glBindFramebufferOES}, {"_glGenRenderbuffersOES", ios_glGenRenderbuffersOES},
  {"_glBindRenderbufferOES", ios_glBindRenderbufferOES}, {"_glRenderbufferStorageOES", ios_glRenderbufferStorageOES},
  {"_glFramebufferRenderbufferOES", ios_glFramebufferRenderbufferOES}, {"_glFramebufferTexture2DOES", ios_glFramebufferTexture2DOES},
  {"_glCheckFramebufferStatusOES", ios_glCheckFramebufferStatusOES}, {"_glGetRenderbufferParameterivOES", ios_glGetRenderbufferParameterivOES},
  {"_glGetIntegerv", ios_glGetIntegerv}, {"_glViewport", ios_glViewport}, {"_glScissor", ios_glScissor},
  {"_glClearColor", ios_glClearColor}, {"_glColor4f", ios_glColor4f}, {"_glOrthof", ios_glOrthof},
  {"_glTranslatef", ios_glTranslatef}, {"_glTexParameterf", ios_glTexParameterf}, {"_glAlphaFunc", ios_glAlphaFunc},
  {"_glBindBuffer", w_glBindBuffer}, {"_glBindTexture", w_glBindTexture}, {"_glBlendFunc", w_glBlendFunc},
  {"_glClear", w_glClear}, {"_glColorPointer", w_glColorPointer}, {"_glCompressedTexImage2D", w_glCompressedTexImage2D},
  {"_glDeleteTextures", w_glDeleteTextures}, {"_glDisable", w_glDisable}, {"_glDisableClientState", w_glDisableClientState},
  {"_glDrawArrays", w_glDrawArrays}, {"_glEnable", w_glEnable}, {"_glEnableClientState", w_glEnableClientState},
  {"_glFlush", w_glFlush}, {"_glGenTextures", w_glGenTextures}, {"_glGetError", w_glGetError},
  {"_glLoadIdentity", w_glLoadIdentity}, {"_glMatrixMode", w_glMatrixMode}, {"_glTexCoordPointer", w_glTexCoordPointer},
  {"_glTexImage2D", w_glTexImage2D}, {"_glTexParameteri", w_glTexParameteri}, {"_glVertexPointer", w_glVertexPointer},
};
uintptr_t gl_resolve_symbol(const char *name) {
  if (strncmp(name, "_gl", 3)) return 0;
  for (unsigned i = 0; i < sizeof(g_gl) / sizeof(g_gl[0]); i++) if (!strcmp(g_gl[i].n, name)) return (uintptr_t)g_gl[i].p;
  return 0;
}
