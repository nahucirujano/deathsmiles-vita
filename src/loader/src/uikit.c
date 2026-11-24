// Minimal UIKit / QuartzCore / EAGL / main loop for the game.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <math.h>
#include <time.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/ctrl.h>
#include "objc.h"
#include "gfx.h"
#include "tstate.h"

typedef struct { float x, y; } CGPoint;
typedef struct { float w, h; } CGSize;
typedef struct { float x, y, w, h; } CGRect;
typedef struct { float a, b, c, d, tx, ty; } CGAffineTransform;
static const CGAffineTransform IDENT = {1, 0, 0, 1, 0, 0};
static Class_t *C_View, *C_Window, *C_VC, *C_Layer, *C_EAGLLayer, *C_Screen, *C_Device;

static double now_s(void) { return sceKernelGetProcessTimeWide() / 1e6; }

// ---------------- CoreGraphics / CoreFoundation ----------------
static double g_cf_base;
SOFTFP static double ios_CFAbsoluteTimeGetCurrent(void) { return g_cf_base + now_s(); }
double cf_now(void) { return g_cf_base + now_s(); }
SOFTFP static CGAffineTransform ios_CGAffineTransformMakeTranslation(float x, float y) { CGAffineTransform t = IDENT; t.tx = x; t.ty = y; return t; }
SOFTFP static CGAffineTransform ios_CGAffineTransformConcat(CGAffineTransform a, CGAffineTransform b) {
  CGAffineTransform r = { a.a * b.a + a.b * b.c, a.a * b.b + a.b * b.d, a.c * b.a + a.d * b.c, a.c * b.b + a.d * b.d,
                          a.tx * b.a + a.ty * b.c + b.tx, a.tx * b.b + a.ty * b.d + b.ty };
  return r;
}
SOFTFP static CGAffineTransform ios_CGAffineTransformRotate(CGAffineTransform t, float ang) {
  float s = sinf(ang), c = cosf(ang);
  CGAffineTransform r = { c * t.a + s * t.c, c * t.b + s * t.d, -s * t.a + c * t.c, -s * t.b + c * t.d, t.tx, t.ty };
  return r;
}

// ---------------- UIScreen / UIDevice / UIApplication ----------------
static int g_screen_w = 320, g_screen_h = 480;     // iPhone 3GS/4 in portrait (points)
static id g_main_screen, g_device;
IMPL static id Screen_main(Class_t *s, SEL c) { if (!g_main_screen) g_main_screen = objc_alloc(C_Screen, 0); return g_main_screen; }
IMPL static CGRect Screen_bounds(id s, SEL c) {
  static int n; if (n++ < 4) dslogv("UIScreen bounds -> %dx%d from game+%#x", g_screen_w, g_screen_h, GAMEADDR(__builtin_return_address(0)));
  CGRect r = {0, 0, g_screen_w, g_screen_h}; return r;
}
IMPL static float Screen_scale(id s, SEL c) { return 1.0f; }
IMPL static id Device_current(Class_t *s, SEL c) { if (!g_device) g_device = objc_alloc(C_Device, 0); return g_device; }
IMPL static id Device_systemVersion(id s, SEL c) { return objc_autorelease_(nsstring_from_cstr("7.1")); /* >= 7: the game enables MFi controllers */ }
IMPL static id Device_model(id s, SEL c) { return objc_autorelease_(nsstring_from_cstr("iPhone")); }
IMPL static id Device_name(id s, SEL c) { return objc_autorelease_(nsstring_from_cstr("PS Vita")); }
IMPL static int Device_idiom(id s, SEL c) { return 0; }
IMPL static int Device_orientation(id s, SEL c) { return 4; }          // UIDeviceOrientationLandscapeLeft
IMPL static int Int4(id s, SEL c) { return 4; }                          // UIInterfaceOrientationLandscapeLeft
IMPL static void Nop(id s, SEL c) {}
IMPL static int Yes(id s, SEL c) { return 1; }
IMPL static int No(id s, SEL c) { return 0; }

// ---------------- CALayer / CAEAGLLayer ----------------
typedef struct { Class_t *isa; id view; id props; } Layer;
IMPL static id Layer_init(Layer *l, SEL c) { return (id)l; }
IMPL static void Layer_setProps(Layer *l, SEL c, id p) { objc_retain_(p); l->props = p; }
IMPL static id Layer_props(Layer *l, SEL c) { return l->props; }

// ---------------- UIView ----------------
typedef struct {
  CGRect bounds; CGPoint center; CGAffineTransform t;
  id superview, subviews, layer, window;
  int hidden, tag; float alpha;
  int ui;   // userInteractionEnabled: 0 unset (per class), 1 yes, 2 no
  // UIKit composition on top of GL
  uint32_t tex; int texw, texh, dirty; id bg; id image;
  id target; SEL action; int pressed;   // UIButton
  uint32_t sig;   // signature of the last drawn content (labels/images without drawRect:)
  float lay_w, lay_h; int laid;   // size layoutSubviews was last called with
  struct LabelSt *label;
} ViewSt;
typedef struct LabelSt { id text, font, color, shadow; float shx, shy; int align, lines, fit; float minsize; } LabelSt;
typedef struct { Class_t *isa; ViewSt *st; } View;
static ViewSt *vst(id v) {
  View *o = (View *)v;
  if (!o->st) { o->st = calloc(1, sizeof(ViewSt)); o->st->t = IDENT; o->st->alpha = 1; o->st->dirty = 1; }
  return o->st;
}
IMPL static Class_t *View_layerClass(Class_t *s, SEL c) { return C_Layer; }
IMPL static id View_initWithFrame(id v, SEL c, CGRect f) {
  ViewSt *s = vst(v);
  s->bounds = (CGRect){0, 0, f.w, f.h}; s->center = (CGPoint){f.x + f.w / 2, f.y + f.h / 2};
  if (!strcmp(class_name(v->isa), "DSMenuHistWindow")) { void hist_hook_c(Class_t *); hist_hook_c(v->isa); }
  dslogv("UIView %s initWithFrame (%d,%d,%d,%d) from game+%#x", class_name(v->isa), (int)f.x, (int)f.y, (int)f.w, (int)f.h, GAMEADDR(__builtin_return_address(0)));
  return v;
}
typedef IMPL id (*InitFrameFn)(id, SEL, CGRect);
static id send_initWithFrame(id v, CGRect r) {
  SEL s = sel_registerName("initWithFrame:");
  return ((InitFrameFn)msg_lookup(v, s))(v, s, r);
}
IMPL static id View_init(id v, SEL c) { CGRect z = {0, 0, 0, 0}; return send_initWithFrame(v, z); }
IMPL static id View_layer(id v, SEL c) {
  ViewSt *s = vst(v);
  if (!s->layer) {
    Class_t *lc = (Class_t *)MSG((id)v->isa, "layerClass");
    if (!lc) lc = C_Layer;
    s->layer = MSG(MSG((id)lc, "alloc"), "init");
    ((Layer *)s->layer)->view = v;
  }
  return s->layer;
}
IMPL static CGRect View_bounds(id v, SEL c) { return vst(v)->bounds; }
IMPL static CGPoint View_center(id v, SEL c) { return vst(v)->center; }
IMPL static CGAffineTransform View_transform(id v, SEL c) { return vst(v)->t; }
IMPL static CGRect View_frame(id v, SEL c) {
  ViewSt *s = vst(v);
  float w = fabsf(s->t.a) * s->bounds.w + fabsf(s->t.c) * s->bounds.h, h = fabsf(s->t.b) * s->bounds.w + fabsf(s->t.d) * s->bounds.h;
  CGRect r = {s->center.x - w / 2 + s->t.tx, s->center.y - h / 2 + s->t.ty, w, h}; return r;
}
static void layout_tree(id v);
static int g_needs_layout;
IMPL static void View_setBounds(id v, SEL c, CGRect b) {
  vst(v)->bounds = b; g_needs_layout = 1;
  dslogv("%s setBounds (%d,%d,%d,%d) from game+%#x", class_name(v->isa), (int)b.x, (int)b.y, (int)b.w, (int)b.h, GAMEADDR(__builtin_return_address(0)));
}
IMPL static void View_setCenter(id v, SEL c, CGPoint p) { vst(v)->center = p; }
IMPL static void View_setTransform(id v, SEL c, CGAffineTransform t) { vst(v)->t = t; dslogv("%s setTransform [%.2f %.2f %.2f %.2f]", class_name(v->isa), t.a, t.b, t.c, t.d); }
IMPL static void View_setFrame(id v, SEL c, CGRect f) {
  ViewSt *s = vst(v);
  s->bounds.w = f.w; s->bounds.h = f.h; s->center = (CGPoint){f.x + f.w / 2 - s->t.tx, f.y + f.h / 2 - s->t.ty};
  g_needs_layout = 1;
}
// Dialogue scenes. The game adds a DSTalkSender view when each scene starts
// and removes it when it ends; while it is visible, gl.c uses the menu bezel and shows the original frame.
static id g_talk_view;
int ui_talk_active(void) {
  id v = g_talk_view; if (!v) return 0;
  ViewSt *s = vst(v);
  return s->superview && !s->hidden && s->alpha > 0.01f;
}
IMPL static void View_addSubview(id v, SEL c, id sub) {
  if (!sub) return;
  ViewSt *s = vst(v);
  if (!s->subviews) s->subviews = MSG((id)objc_getClass("NSMutableArray"), "new");
  MSG(sub, "removeFromSuperview");
  MSG(s->subviews, "addObject:", sub);
  vst(sub)->superview = v;
  dslogv("%s addSubview %s", class_name(v->isa), class_name(sub->isa));
  if (!strcmp(class_name(sub->isa), "DSTalkSender")) { g_talk_view = sub; dslog("dialogue: scene started (DSTalkSender)"); }
  g_needs_layout = 1;
}
IMPL static void View_removeFromSuperview(id v, SEL c) {
  ViewSt *s = vst(v);
  if (!s->superview) return;
  id sup = s->superview; s->superview = NULL;
  if (v == g_talk_view) { g_talk_view = NULL; dslog("dialogue: scene ended (DSTalkSender removed)"); }
  objc_retain_(v); MSG(vst(sup)->subviews, "removeObject:", v); objc_autorelease_(v);
}
IMPL static id View_superview(id v, SEL c) { return vst(v)->superview; }
IMPL static int Zero(id v, SEL c) { return 0; }
IMPL static id View_subviews(id v, SEL c) { ViewSt *s = vst(v); return s->subviews ? s->subviews : MSG((id)objc_getClass("NSArray"), "array"); }
IMPL static void View_setHidden(id v, SEL c, int h) { vst(v)->hidden = h; }
IMPL static int View_hidden(id v, SEL c) { return vst(v)->hidden; }
IMPL static void View_setAlpha(id v, SEL c, float a) { vst(v)->alpha = a; }
IMPL static float View_alpha(id v, SEL c) { return vst(v)->alpha; }
IMPL static void View_setTag(id v, SEL c, int t) { vst(v)->tag = t; }
IMPL static int View_tag(id v, SEL c) { return vst(v)->tag; }
IMPL static void View_setIgnored(id v, SEL c, int x) {}
static Class_t *C_LabelK, *C_ImageViewK, *C_Button;
static int view_ui(id v) {   // as on iOS: UILabel and UIImageView ignore touches unless enabled
  ViewSt *s = vst(v);
  if (s->ui) return s->ui == 1;
  for (Class_t *k = v->isa; k; k = k->superclass) if (k == C_LabelK || k == C_ImageViewK) return 0;
  return 1;
}
IMPL static void View_setUI(id v, SEL c, int x) { vst(v)->ui = x ? 1 : 2; }
IMPL static int View_isUI(id v, SEL c) { return view_ui(v); }
IMPL static float View_one(id v, SEL c) { return 1.0f; }
// As on iOS, layoutSubviews runs only the first time or when the view's SIZE changes. Before, it was called on
// the whole tree whenever any view moved; the game moves the menu cursor with setFrame: every frame
// and -[EAGLView layoutSubviews] draws the scene again -> two draws per loop iteration (~50 % of the
// time in "layout"), menus at 48–56 FPS and the controller read at ~30 Hz.
static void layout_tree(id v) {
  if (!v || vst(v)->hidden < 0) return;
  ViewSt *s = vst(v);
  if (!s->laid || s->lay_w != s->bounds.w || s->lay_h != s->bounds.h) {
    s->laid = 1; s->lay_w = s->bounds.w; s->lay_h = s->bounds.h;
    MSG(v, "layoutSubviews");
  }
  if (!s->subviews) return;
  uint32_t n = (uint32_t)MSG(s->subviews, "count");
  for (uint32_t i = 0; i < n; i++) layout_tree(MSG(s->subviews, "objectAtIndex:", i));
}

// ---------------- UIImage (images are not decoded here) ----------------
typedef struct { Class_t *isa; id name; } Img;
IMPL static id Img_named(Class_t *c, SEL s, id name) {
  dslog("[not-implemented] UIImage %s (returning an empty image)", name ? nsstring_utf8(name) : "(nil)");
  Img *i = (Img *)objc_alloc(c, 0); i->name = objc_retain_(name); return objc_autorelease_((id)i);
}
IMPL static CGSize Img_size(Img *i, SEL s) { CGSize z = {0, 0}; return z; }
IMPL static void *Img_cg(Img *i, SEL s) { return NULL; }
IMPL static id View_initWithImage(id v, SEL c, id img) { CGRect z = {0, 0, 0, 0}; return send_initWithFrame(v, z); }

// ---- text/colours: support objects ----
typedef struct { Class_t *isa; float r, g, b, a; } Color;
static Class_t *C_Color, *C_Font;
IMPL static id Color_rgba(Class_t *c, SEL s, float r, float g, float b, float a) { Color *k = (Color *)objc_alloc(C_Color, 0); k->r = r; k->g = g; k->b = b; k->a = a; return objc_autorelease_((id)k); }
IMPL static id Color_init(Color *k, SEL s, float r, float g, float b, float a) { k->r = r; k->g = g; k->b = b; k->a = a; return (id)k; }
IMPL static id Color_white(Class_t *c, SEL s) { return Color_rgba(c, s, 1, 1, 1, 1); }
IMPL static id Color_black(Class_t *c, SEL s) { return Color_rgba(c, s, 0, 0, 0, 1); }
IMPL static id Color_clear(Class_t *c, SEL s) { return Color_rgba(c, s, 0, 0, 0, 0); }
typedef struct { Class_t *isa; float size; } Font;
IMPL static id Font_named(Class_t *c, SEL s, id name, float size) { Font *f = (Font *)objc_alloc(C_Font, 0); f->size = size; return objc_autorelease_((id)f); }
IMPL static id Font_system(Class_t *c, SEL s, float size) { Font *f = (Font *)objc_alloc(C_Font, 0); f->size = size; return objc_autorelease_((id)f); }
IMPL static CGSize Str_sizeWithFont(id str, SEL s, Font *f) {
  float sz = f && f->isa == C_Font ? f->size : 12; const char *u = nsstring_utf8(str);
  CGSize r = {strlen(u ? u : "") * sz * 0.55f, sz * 1.2f}; return r;
}
static id view_with_tag(id v, int tag) {
  if (!v) return NULL;
  if (vst(v)->tag == tag) return v;
  id subs = vst(v)->subviews; if (!subs) return NULL;
  uint32_t n = (uint32_t)MSG(subs, "count");
  for (uint32_t i = 0; i < n; i++) { id r = view_with_tag(MSG(subs, "objectAtIndex:", i), tag); if (r) return r; }
  return NULL;
}
// By default UIView forwards touches to the next "responder" (its superview), as on iOS: so a
// [super touchesBegan:] in a game view (e.g. messageLabelView) reaches EAGLView.
IMPL static void View_touchNop(id v, SEL c, id t, id e) {
  id sup = vst(v)->superview;
  if (sup) MSG(sup, c, t, e);
}
IMPL static id View_viewWithTag(id v, SEL c, int tag) { return view_with_tag(v, tag); }
IMPL static id Ret_nil(id o, SEL c) { if (o && !class_is_meta(o->isa)) objc_free_object(o); return NULL; }

IMPL static void View_touchNop(id v, SEL c, id t, id e);
// first view (depth-first from the key window) whose class implements sel
static id find_resp(id v, SEL s) {
  if (!v) return NULL;
  ViewSt *st = vst(v);
  if (st->subviews) {
    uint32_t n = (uint32_t)MSG(st->subviews, "count");
    for (uint32_t i = n; i-- > 0;) { id r = find_resp(MSG(st->subviews, "objectAtIndex:", i), s); if (r) return r; }
  }
  IMP imp = class_lookup(v->isa, s);
  return imp && imp != (IMP)View_touchNop ? v : NULL;    // ignore UIView's empty implementation
}
static id g_key_window;
id view_find_responder(const char *sel) { return find_resp(g_key_window, sel_registerName(sel)); }

// ---------------- UIWindow ----------------
typedef struct { id root; } WinExtra;
static id g_root_vc;
IMPL static void Win_makeKeyAndVisible(id w, SEL c) { g_key_window = w; dslogv("makeKeyAndVisible"); g_needs_layout = 1; }
IMPL static void Win_setRoot(id w, SEL c, id vc) {
  g_root_vc = objc_retain_(vc);
  id view = MSG(vc, "view");
  MSG(w, "addSubview:", view);
  if (class_responds(vc->isa, sel_registerName("viewWillAppear:"))) MSG(vc, "viewWillAppear:", 0);
  if (class_responds(vc->isa, sel_registerName("viewDidAppear:"))) MSG(vc, "viewDidAppear:", 0);
}
IMPL static id Win_root(id w, SEL c) { return g_root_vc; }
IMPL static id App_keyWindow(id a, SEL c) { return g_key_window; }

// ---------------- UIViewController ----------------
typedef struct { Class_t *isa; id view; } VC;
IMPL static id VC_initNib(VC *vc, SEL c, id nib, id bundle) { return MSG((id)vc, "init"); }
IMPL static id VC_init(VC *vc, SEL c) { return (id)vc; }
IMPL static id VC_view(VC *vc, SEL c) {
  if (!vc->view) {
    MSG((id)vc, "loadView");
    if (!vc->view) { CGRect r = {0, 0, g_screen_w, g_screen_h}; vc->view = send_initWithFrame(MSG((id)C_View, "alloc"), r); }
    MSG((id)vc, "viewDidLoad");
  }
  return vc->view;
}
IMPL static void VC_setView(VC *vc, SEL c, id v) { objc_retain_(v); vc->view = v; }
IMPL static void VC_loadView(VC *vc, SEL c) {}
IMPL static int VC_interfaceOrientation(VC *vc, SEL c) { return 4; }
IMPL static void VC_present(VC *vc, SEL c, id other, int anim) { dslog("[falta-implementar] presentModalViewController %s", other ? class_name(other->isa) : "nil"); }

// ---------------- EAGLContext ----------------
IMPL static id Ctx_initWithAPI(id ctx, SEL c, int api) {
  if (api != 1) { dslog("EAGLContext initWithAPI:%d not supported (ES1 only)", api); objc_free_object(ctx); return NULL; }
  dslog("EAGLContext initWithAPI:%d", api); gfx_init(); return ctx;
}
IMPL static id Ctx_initShare(id ctx, SEL c, int api, id group) { return Ctx_initWithAPI(ctx, c, api); }
static id g_sharegroup;
IMPL static id Ctx_sharegroup(id ctx, SEL c) { if (!g_sharegroup) g_sharegroup = objc_alloc(g_cls_NSObject, 0); return g_sharegroup; }
IMPL static int Ctx_setCurrent(Class_t *s, SEL c, id ctx) { ts_get()->ctx = ctx; return 1; }
IMPL static id Ctx_current(Class_t *s, SEL c) { return ts_get()->ctx; }
IMPL static int Ctx_storage(id ctx, SEL c, int target, Layer *layer) {
  int w = 480, h = 320;
  if (layer && layer->view) { CGRect b = vst(layer->view)->bounds; w = (int)b.w; h = (int)b.h; }
  if (w <= 0 || h <= 0) { w = 480; h = 320; }
  gfx_set_screen(w, h);
  return 1;
}
IMPL static int Ctx_present(id ctx, SEL c, int target) { gfx_present(); return 1; }

// ---------------- NSTimer, CADisplayLink, NSRunLoop ----------------
typedef struct { Class_t *isa; id target; SEL sel; id info; double interval, next; int repeats, valid, scheduled; } Timer;
typedef struct { Class_t *isa; id target; SEL sel; int interval, paused, valid, count; double ts; } Link;
#define MAXT 64
static Timer *g_timers[MAXT]; static Link *g_links[16];
static Class_t *C_Timer, *C_Link;
static void timer_schedule(Timer *t) {
  if (t->scheduled) return;
  for (int i = 0; i < MAXT; i++) if (!g_timers[i]) { g_timers[i] = (Timer *)objc_retain_((id)t); t->scheduled = 1; return; }
  dslog("[CPU-bound] [error] too many timers");
}
IMPL static id Timer_new(Class_t *s, SEL c, double iv, id target, SEL sel, id info, int rep) {
  Timer *t = (Timer *)objc_alloc(C_Timer, 0);
  t->target = objc_retain_(target); t->sel = sel; t->info = objc_retain_(info); t->interval = iv < 0.0001 ? 0.0001 : iv;
  t->next = now_s() + t->interval; t->repeats = rep; t->valid = 1;
  return objc_autorelease_((id)t);
}
IMPL static id Timer_scheduled(Class_t *s, SEL c, double iv, id target, SEL sel, id info, int rep) {
  Timer *t = (Timer *)Timer_new(s, c, iv, target, sel, info, rep);
  dslogv("NSTimer %.4f s -> -[%s %s]%s", iv, class_name(target ? target->isa : NULL), sel, rep ? " (repite)" : "");
  timer_schedule(t); return (id)t;
}
IMPL static void Timer_invalidate(Timer *t, SEL c) { t->valid = 0; }
IMPL static int Timer_isValid(Timer *t, SEL c) { return t->valid; }
IMPL static id Timer_userInfo(Timer *t, SEL c) { return t->info; }
IMPL static void Timer_fire(Timer *t, SEL c) { MSG(t->target, t->sel, t); }
IMPL static id Link_new(Class_t *s, SEL c, id target, SEL sel) {
  Link *l = (Link *)objc_alloc(C_Link, 0);
  l->target = objc_retain_(target); l->sel = sel; l->interval = 1; l->valid = 1;
  dslogv("CADisplayLink -> -[%s %s]", class_name(target->isa), sel);
  return objc_autorelease_((id)l);
}
IMPL static void Link_setInterval(Link *l, SEL c, int n) { l->interval = n < 1 ? 1 : n; dslogv("CADisplayLink frameInterval=%d", l->interval); }
IMPL static void Link_add(Link *l, SEL c, id rl, id mode) {
  for (int i = 0; i < 16; i++) if (!g_links[i]) { g_links[i] = (Link *)objc_retain_((id)l); return; }
}
IMPL static void Link_invalidate(Link *l, SEL c) { l->valid = 0; }
IMPL static void Link_setPaused(Link *l, SEL c, int p) { l->paused = p; }
IMPL static double Link_timestamp(Link *l, SEL c) { return l->ts; }
IMPL static double Link_duration(Link *l, SEL c) { return 1.0 / 60.0; }
static id g_runloop;
IMPL static id RL_current(Class_t *s, SEL c) { if (!g_runloop) g_runloop = objc_alloc(s, 0); return g_runloop; }
IMPL static void RL_addTimer(id rl, SEL c, Timer *t, id mode) { timer_schedule(t); }
IMPL static void Obj_performAfterDelay(id self, SEL c, SEL s, id arg, double delay) {
  Timer *t = (Timer *)objc_alloc(C_Timer, 0);
  t->target = objc_retain_(self); t->sel = s; t->info = objc_retain_(arg);
  t->interval = delay; t->next = now_s() + delay; t->repeats = -1; t->valid = 1;   // -1 = performSelector:afterDelay:
  timer_schedule(t); objc_release_((id)t);
}

// message queue for the main thread (performSelectorOnMainThread, notifications from threads)
typedef struct { id t; SEL s; id a; } Post;
static Post g_posts[256]; static int g_post_n; static SceKernelLwMutexWork g_post_mx; static int g_post_ok;
void notify_post(id note);
void runloop_post(id target, SEL sel, id arg) {
  if (!g_post_ok) { sceKernelCreateLwMutex(&g_post_mx, "post", 0, 0, NULL); g_post_ok = 1; }
  sceKernelLockLwMutex(&g_post_mx, 1, NULL);
  if (g_post_n < 256) g_posts[g_post_n++] = (Post){objc_retain_(target), sel, arg};
  else dslog("[CPU-bound] [error] main thread queue full");
  sceKernelUnlockLwMutex(&g_post_mx, 1);
}
static void runloop_drain_posts(void) {
  if (!g_post_ok || !g_post_n) return;
  Post local[256]; int n;
  sceKernelLockLwMutex(&g_post_mx, 1, NULL); n = g_post_n; memcpy(local, g_posts, n * sizeof(Post)); g_post_n = 0; sceKernelUnlockLwMutex(&g_post_mx, 1);
  for (int i = 0; i < n; i++) {
    if (!local[i].t && !strcmp(local[i].s, "__post_note")) { { void files_note_delivered(void); files_note_delivered(); } notify_post(local[i].a); objc_release_(local[i].a); continue; }
    if (!local[i].t && !strcmp(local[i].s, "__block")) { extern void block_invoke_and_release(id); block_invoke_and_release(local[i].a); continue; }
    MSG(local[i].t, local[i].s, local[i].a); objc_release_(local[i].t); objc_release_(local[i].a);
  }
}
static SceUID g_main_thread;
IMPL static void Obj_performOnMain(id self, SEL c, SEL s, id arg, int wait) {
  if (sceKernelGetThreadId() == g_main_thread) { MSG(self, s, arg); return; }
  runloop_post(self, s, objc_retain_(arg));
  if (wait) { dslog("[warning] performSelectorOnMainThread waitUntilDone from another thread: approximate wait"); sceKernelDelayThread(20000); }
}

void runloop_run(void) {
  g_main_thread = sceKernelGetThreadId();
  dslog("main loop: timers and CADisplayLink active. START+SELECT to exit.");
  // Measures what runs on the main thread OUTSIDE drawView: (timers, cross-thread notices,
  // layout, controller). In menus there were 48–56 FPS windows with drawView: always <=16.7 ms: the time was spent
  // outside drawView:. Every 5 s: total and maximum per part, and the slowest timer (verbose builds only).
  static uint64_t st_tm, st_tm_mx, st_post, st_post_mx, st_lay, st_in, st_in_mx, st_last; static SEL st_slow; static uint64_t st_slow_us;
  for (;;) {
    void *pool = pool_push();
    double t = now_s();
    uint64_t q0 = sceKernelGetProcessTimeWide();
    for (int i = 0; i < MAXT; i++) {
      Timer *tm = g_timers[i];
      if (!tm) continue;
      if (!tm->valid) { g_timers[i] = NULL; objc_release_((id)tm); continue; }
      if (t < tm->next) continue;
      uint64_t tq = sceKernelGetProcessTimeWide();
      if (tm->repeats == -1) { MSG(tm->target, tm->sel, tm->info); tm->valid = 0; }
      else MSG(tm->target, tm->sel, (id)tm);
      tq = sceKernelGetProcessTimeWide() - tq; if (tq > st_slow_us) { st_slow_us = tq; st_slow = tm->sel; }
      if (tm->repeats == -1) continue;
      if (tm->repeats) { tm->next += tm->interval; if (tm->next < t) tm->next = t + tm->interval; } else tm->valid = 0;
    }
    uint64_t q1 = sceKernelGetProcessTimeWide();
    runloop_drain_posts();
    uint64_t q2 = sceKernelGetProcessTimeWide();
    if (g_needs_layout && g_key_window) { g_needs_layout = 0; layout_tree(g_key_window); }
    uint64_t q3 = sceKernelGetProcessTimeWide();
    st_tm += q1 - q0; if (q1 - q0 > st_tm_mx) st_tm_mx = q1 - q0;
    st_post += q2 - q1; if (q2 - q1 > st_post_mx) st_post_mx = q2 - q1;
    st_lay += q3 - q2;
    int drew = 0;
    for (int i = 0; i < 16; i++) {
      Link *l = g_links[i];
      if (!l) continue;
      if (!l->valid) { g_links[i] = NULL; continue; }
      if (l->paused) continue;
      if (++l->count % l->interval) continue;
      l->ts = t;
      uint64_t t0 = sceKernelGetProcessTimeWide();
      MSG(l->target, l->sel, (id)l); drew = 1;
      static uint64_t acc, mx, n, last;
      uint64_t dt = sceKernelGetProcessTimeWide() - t0; acc += dt; if (dt > mx) mx = dt; n++;
      if (t0 - last > 5000000) {
        { void gfx_lock_report(void), files_read_report(void); if (last) { gfx_lock_report(); files_read_report(); } }
        if (last && (DS_VERBOSE || acc / n > 16667)) dslog("%s drawView: average %.2f ms, max %.2f ms (budget 16.7 ms)%s",
                        acc / n > 16667 ? "[CPU-bound]" : "[OK]", acc / 1000.0 / n, mx / 1000.0,
                        acc / n > 16667 ? " [CPU-bound: main-thread stall]" : "");
        acc = mx = n = 0; last = t0;
      }
    }
    pool_pop(pool);
    { extern void input_poll_buttons(void), input_poll_touch(void), input_announce_controller(void);
      uint64_t i0 = sceKernelGetProcessTimeWide();
      static int frames; input_poll_buttons(); input_poll_touch(); if (++frames == 120) input_announce_controller();
      uint64_t di = sceKernelGetProcessTimeWide() - i0; st_in += di; if (di > st_in_mx) st_in_mx = di; }
    { uint64_t now = sceKernelGetProcessTimeWide();
      if (now - st_last > 5000000) {
        if (st_last) {
          uint64_t out = st_tm + st_post + st_lay + st_in;
          dslogv("%s outside drawView: %.0f ms in 5 s (timers %.0f, max %.1f; notices %.0f, max %.1f; layout %.0f; controller %.0f, max %.1f). Slowest timer: %s %.1f ms",
                out > 250000 ? "[CPU-bound: main thread, work outside drawing]" : "[OK]", out / 1000.0,
                st_tm / 1000.0, st_tm_mx / 1000.0, st_post / 1000.0, st_post_mx / 1000.0, st_lay / 1000.0,
                st_in / 1000.0, st_in_mx / 1000.0, st_slow ? (const char *)st_slow : "-", st_slow_us / 1000.0);
        }
        st_tm = st_tm_mx = st_post = st_post_mx = st_lay = st_in = st_in_mx = 0; st_slow = 0; st_slow_us = 0; st_last = now;
      } }
    SceCtrlData pad; sceCtrlPeekBufferPositive(0, &pad, 1);
    if ((pad.buttons & (SCE_CTRL_START | SCE_CTRL_SELECT)) == (SCE_CTRL_START | SCE_CTRL_SELECT)) { dslog("exit requested (START+SELECT)"); dslog_flush(); sceKernelExitProcess(0); }
    if (!drew) sceKernelDelayThread(4000);
  }
}

uintptr_t uikit_resolve_symbol(const char *name) {
  static const struct { const char *n; void *p; } t[] = {
    {"_CFAbsoluteTimeGetCurrent", ios_CFAbsoluteTimeGetCurrent},
    {"_CGAffineTransformMakeTranslation", ios_CGAffineTransformMakeTranslation},
    {"_CGAffineTransformConcat", ios_CGAffineTransformConcat}, {"_CGAffineTransformRotate", ios_CGAffineTransformRotate},
  };
  for (unsigned i = 0; i < sizeof(t) / sizeof(t[0]); i++) if (!strcmp(t[i].n, name)) return (uintptr_t)t[i].p;
  return gl_resolve_symbol(name);
}

void uikit_text_init(Class_t *LBL);
// ---- subview order (DSUIOrderWindow show::: uses bringSubviewToFront:) ----
static void subview_move(id v, id sub, int where /* 0 = back, -1 = forward, other = index+1 */) {
  ViewSt *s = vst(v); if (!sub || !s->subviews || vst(sub)->superview != v) return;
  objc_retain_(sub); MSG(s->subviews, "removeObject:", sub);
  uint32_t n = (uint32_t)MSG(s->subviews, "count");
  if (where < 0 || (uint32_t)(where - 1) > n) MSG(s->subviews, "addObject:", sub);
  else MSG(s->subviews, "insertObject:atIndex:", sub, where == 0 ? 0 : where - 1);
  objc_release_(sub);
}
IMPL static void View_toFront(id v, SEL c, id sub) { subview_move(v, sub, -1); }
IMPL static void View_toBack(id v, SEL c, id sub) { subview_move(v, sub, 0); }
static void insert_common(id v, id sub, int idx) {
  if (!sub) return;
  MSG(v, "addSubview:", sub);                        // appends it at the end
  if (idx >= 0) subview_move(v, sub, idx + 1);
}
static int subview_index(id v, id other) {
  ViewSt *s = vst(v); if (!s->subviews || !other) return -1;
  uint32_t n = (uint32_t)MSG(s->subviews, "count");
  for (uint32_t i = 0; i < n; i++) if (MSG(s->subviews, "objectAtIndex:", i) == other) return (int)i;
  return -1;
}
IMPL static void View_insertAt(id v, SEL c, id sub, int idx) { insert_common(v, sub, idx); }
IMPL static void View_insertAbove(id v, SEL c, id sub, id other) { int i = subview_index(v, other); insert_common(v, sub, i < 0 ? -1 : i + 1); }
IMPL static void View_insertBelow(id v, SEL c, id sub, id other) { int i = subview_index(v, other); insert_common(v, sub, i < 0 ? -1 : i); }
static void button_init(Class_t *B);
void uikit_init(void) {
  g_cf_base = (double)time(NULL) - 978307200.0 - now_s();
  Class_t *O = g_cls_NSObject;
  class_add(O, "performSelector:withObject:afterDelay:", Obj_performAfterDelay);
  class_add(O, "performSelectorOnMainThread:withObject:waitUntilDone:", Obj_performOnMain);

  C_Screen = objc_class_new("UIScreen", O, 4);
  class_add_meta(C_Screen, "mainScreen", Screen_main);
  class_add(C_Screen, "bounds", Screen_bounds); class_add(C_Screen, "applicationFrame", Screen_bounds);
  class_add(C_Screen, "scale", Screen_scale);
  C_Device = objc_class_new("UIDevice", O, 4);
  class_add_meta(C_Device, "currentDevice", Device_current);
  class_add(C_Device, "systemVersion", Device_systemVersion); class_add(C_Device, "model", Device_model);
  class_add(C_Device, "localizedModel", Device_model); class_add(C_Device, "name", Device_name);
  class_add(C_Device, "userInterfaceIdiom", Device_idiom); class_add(C_Device, "orientation", Device_orientation);
  class_add(C_Device, "beginGeneratingDeviceOrientationNotifications", Nop);
  class_add(C_Device, "isMultitaskingSupported", No);

  Class_t *A = objc_getClass("UIApplication");
  const char *noop1[] = {"setIdleTimerDisabled:", "registerForRemoteNotificationTypes:", "setStatusBarOrientation:",
                         "setStatusBarOrientation:animated:", "setStatusBarHidden:", "setStatusBarHidden:animated:",
                         "setStatusBarHidden:withAnimation:", "setApplicationIconBadgeNumber:", "setStatusBarStyle:",
                         "beginIgnoringInteractionEvents", "endIgnoringInteractionEvents"};
  for (unsigned i = 0; i < sizeof(noop1) / sizeof(noop1[0]); i++) class_add(A, noop1[i], Nop);
  class_add(A, "statusBarOrientation", Int4); class_add(A, "keyWindow", App_keyWindow);
  class_add(A, "applicationState", No); class_add(A, "openURL:", No); class_add(A, "canOpenURL:", No);

  C_Layer = objc_class_new("CALayer", O, sizeof(Layer));
  C_EAGLLayer = objc_class_new("CAEAGLLayer", C_Layer, sizeof(Layer));
  class_add(C_Layer, "init", Layer_init);
  class_add(C_Layer, "setDrawableProperties:", Layer_setProps); class_add(C_Layer, "drawableProperties", Layer_props);
  const char *lnop[] = {"setOpaque:", "setContentsScale:", "setMasksToBounds:", "setNeedsDisplay", "setBackgroundColor:"};
  for (unsigned i = 0; i < sizeof(lnop) / sizeof(lnop[0]); i++) class_add(C_Layer, lnop[i], View_setIgnored);

  C_View = objc_class_new("UIView", O, sizeof(View));
  class_add_meta(C_View, "layerClass", View_layerClass);
  class_add(C_View, "initWithFrame:", View_initWithFrame); class_add(C_View, "init", View_init);
  class_add(C_View, "layer", View_layer); class_add(C_View, "frame", View_frame); class_add(C_View, "bounds", View_bounds);
  class_add(C_View, "center", View_center); class_add(C_View, "transform", View_transform);
  class_add(C_View, "setFrame:", View_setFrame); class_add(C_View, "setBounds:", View_setBounds);
  class_add(C_View, "setCenter:", View_setCenter); class_add(C_View, "setTransform:", View_setTransform);
  class_add(C_View, "addSubview:", View_addSubview); class_add(C_View, "removeFromSuperview", View_removeFromSuperview);
  class_add(C_View, "superview", View_superview); class_add(C_View, "nextResponder", View_superview); class_add(C_View, "subviews", View_subviews);
  class_add(C_View, "setHidden:", View_setHidden); class_add(C_View, "isHidden", View_hidden);
  class_add(C_View, "setAlpha:", View_setAlpha); class_add(C_View, "alpha", View_alpha);
  class_add(C_View, "setTag:", View_setTag); class_add(C_View, "tag", View_tag);
  class_add(C_View, "contentScaleFactor", View_one); class_add(C_View, "layoutSubviews", Nop);
  class_add(C_View, "setUserInteractionEnabled:", View_setUI); class_add(C_View, "isUserInteractionEnabled", View_isUI);
  const char *vnop[] = {"setOpaque:", "setMultipleTouchEnabled:", "setExclusiveTouch:",
                        "setAutoresizingMask:", "setAutoresizesSubviews:", "setContentScaleFactor:",
                        "setNeedsLayout", "setClipsToBounds:",
                        "setContentMode:"};
  for (unsigned i = 0; i < sizeof(vnop) / sizeof(vnop[0]); i++) class_add(C_View, vnop[i], View_setIgnored);

  const char *subviews_[] = {"UIImageView", "UILabel", "UIButton", "UIActivityIndicatorView", "UITextField",
                             "UITextView", "UIScrollView", "UIControl"};
  for (unsigned i = 0; i < sizeof(subviews_) / sizeof(subviews_[0]); i++) objc_class_new(subviews_[i], C_View, sizeof(View));
  Class_t *IMG = objc_class_new("UIImage", O, sizeof(Img));
  class_add_meta(IMG, "imageNamed:", Img_named); class_add_meta(IMG, "imageWithContentsOfFile:", Img_named);
  class_add(IMG, "size", Img_size); class_add(IMG, "CGImage", Img_cg);
  C_Color = objc_class_new("UIColor", O, sizeof(Color)); C_Font = objc_class_new("UIFont", O, sizeof(Font));
  class_add_meta(C_Color, "colorWithRed:green:blue:alpha:", Color_rgba); class_add_meta(C_Color, "whiteColor", Color_white);
  class_add_meta(C_Color, "blackColor", Color_black); class_add_meta(C_Color, "darkGrayColor", Color_black);
  class_add(C_Color, "initWithRed:green:blue:alpha:", Color_init); class_add_meta(C_Color, "clearColor", Color_clear);
  class_add_meta(C_Font, "fontWithName:size:", Font_named); class_add_meta(C_Font, "systemFontOfSize:", Font_system);
  class_add_meta(C_Font, "boldSystemFontOfSize:", Font_system);
  class_add(C_View, "viewWithTag:", View_viewWithTag);
  const char *tnop[] = {"touchesBegan:withEvent:", "touchesMoved:withEvent:", "touchesEnded:withEvent:", "touchesCancelled:withEvent:"};
  for (unsigned i = 0; i < 4; i++) class_add(C_View, tnop[i], View_touchNop);
  Class_t *LBL = objc_getClass("UILabel"); C_LabelK = LBL; C_ImageViewK = objc_getClass("UIImageView");
  class_add(LBL, "setLineBreakMode:", View_setIgnored);
  uikit_text_init(LBL);
  { void uikit_alert_init(void); uikit_alert_init(); }
  class_add(C_View, "bringSubviewToFront:", View_toFront); class_add(C_View, "sendSubviewToBack:", View_toBack);
  class_add(C_View, "insertSubview:atIndex:", View_insertAt);
  class_add(C_View, "insertSubview:aboveSubview:", View_insertAbove); class_add(C_View, "insertSubview:belowSubview:", View_insertBelow);
  button_init(objc_getClass("UIButton"));
  Class_t *REQ = objc_class_new("NSMutableURLRequest", O, 4);
  const char *rnop[] = {"setURL:", "setTimeoutInterval:", "setHTTPShouldHandleCookies:", "setHTTPMethod:", "setHTTPBody:",
                        "setCachePolicy:", "setValue:forHTTPHeaderField:"};
  for (unsigned i = 0; i < sizeof(rnop) / sizeof(rnop[0]); i++) class_add(REQ, rnop[i], View_setIgnored);
  Class_t *CONN = objc_class_new("NSURLConnection", O, 4);
  class_add(CONN, "initWithRequest:delegate:", Ret_nil);   // no network: the connection is not created
  C_Window = objc_class_new("UIWindow", C_View, sizeof(View));
  class_add(C_Window, "makeKeyAndVisible", Win_makeKeyAndVisible); class_add(C_Window, "setRootViewController:", Win_setRoot);
  class_add(C_Window, "rootViewController", Win_root); class_add(C_Window, "makeKeyWindow", Win_makeKeyAndVisible);

  C_VC = objc_class_new("UIViewController", O, sizeof(VC));
  class_add(C_VC, "initWithNibName:bundle:", VC_initNib); class_add(C_VC, "init", VC_init);
  class_add(C_VC, "view", VC_view); class_add(C_VC, "setView:", VC_setView); class_add(C_VC, "loadView", VC_loadView);
  class_add(C_VC, "viewDidLoad", Nop); class_add(C_VC, "interfaceOrientation", VC_interfaceOrientation);
  class_add(C_VC, "presentModalViewController:animated:", VC_present);
  class_add(C_VC, "presentViewController:animated:completion:", VC_present);
  class_add(C_VC, "viewWillAppear:", View_setIgnored); class_add(C_VC, "viewDidAppear:", View_setIgnored);
  class_add(C_VC, "isViewLoaded", Yes);

  Class_t *E = objc_class_new("EAGLContext", O, 8);
  class_add(E, "initWithAPI:", Ctx_initWithAPI); class_add(E, "initWithAPI:sharegroup:", Ctx_initShare);
  class_add(E, "sharegroup", Ctx_sharegroup);
  class_add_meta(E, "setCurrentContext:", Ctx_setCurrent); class_add_meta(E, "currentContext", Ctx_current);
  class_add(E, "renderbufferStorage:fromDrawable:", Ctx_storage); class_add(E, "presentRenderbuffer:", Ctx_present);

  C_Timer = objc_class_new("NSTimer", O, sizeof(Timer));
  class_add_meta(C_Timer, "scheduledTimerWithTimeInterval:target:selector:userInfo:repeats:", Timer_scheduled);
  class_add_meta(C_Timer, "timerWithTimeInterval:target:selector:userInfo:repeats:", Timer_new);
  class_add(C_Timer, "invalidate", Timer_invalidate); class_add(C_Timer, "isValid", Timer_isValid);
  class_add(C_Timer, "userInfo", Timer_userInfo); class_add(C_Timer, "fire", Timer_fire);
  C_Link = objc_class_new("CADisplayLink", O, sizeof(Link));
  class_add_meta(C_Link, "displayLinkWithTarget:selector:", Link_new);
  class_add(C_Link, "setFrameInterval:", Link_setInterval); class_add(C_Link, "addToRunLoop:forMode:", Link_add);
  class_add(C_Link, "invalidate", Link_invalidate); class_add(C_Link, "setPaused:", Link_setPaused);
  class_add(C_Link, "timestamp", Link_timestamp); class_add(C_Link, "duration", Link_duration);
  Class_t *R = objc_class_new("NSRunLoop", O, 4);
  class_add_meta(R, "currentRunLoop", RL_current); class_add_meta(R, "mainRunLoop", RL_current);
  class_add(R, "addTimer:forMode:", RL_addTimer);
}

// ======================= Text, UIKit drawing and composition on top of GL =======================
#include "font.h"
void *cg_current(void); void cg_push(void *c); void cg_pop(void); void *cg_new_uikit(int w, int h);
uint8_t *cg_data(void *ctx, int *w, int *h, int *pitch); void cg_free(void *ctx);
void cg_set_fill(void *ctx, uint32_t rgba); uint32_t cg_get_fill(void *ctx);
void cg_text(void *ctx, const char *t, float x, float y, float boxw, float size, int align, float maxw, int maxlines);
void cg_image(void *ctx, void *uiimage, float x, float y, float w, float h, float alpha);
int cg_uiimage_size(void *uiimage, int *w, int *h);

static uint32_t color_rgba(id c) {
  if (!c || c->isa != C_Color) return 0;
  Color *k = (Color *)c;
  return ((uint32_t)(k->r * 255) << 24) | ((uint32_t)(k->g * 255) << 16) | ((uint32_t)(k->b * 255) << 8) | (uint32_t)(k->a * 255);
}
static float font_size(id f) { return f && f->isa == C_Font ? ((Font *)f)->size : 17.0f; }
static LabelSt *lst(id v) { ViewSt *s = vst(v); if (!s->label) { s->label = calloc(1, sizeof(LabelSt)); s->label->lines = 1; } return s->label; }
static void dirty(id v) { vst(v)->dirty = 1; }

// ---- UIColor ----
IMPL static void Color_set(id c, SEL s) { cg_set_fill(cg_current(), color_rgba(c)); }
// ---- UIView ----
IMPL static void View_setNeedsDisplay(id v, SEL c) { dirty(v); }
IMPL static void View_setNeedsDisplayRect(id v, SEL c, CGRect r) { dirty(v); }
IMPL static void View_setBg(id v, SEL c, id col) { ViewSt *s = vst(v); objc_retain_(col); objc_release_(s->bg); s->bg = col; dirty(v); }
IMPL static id View_bg(id v, SEL c) { return vst(v)->bg; }
IMPL static void View_drawRectNop(id v, SEL c, CGRect r) {}
// ---- UILabel ----
IMPL static void Lbl_setText(id v, SEL c, id t) { LabelSt *l = lst(v); id n = t ? MSG(t, "copy") : NULL; objc_release_(l->text); l->text = n; dirty(v); }
IMPL static id Lbl_text(id v, SEL c) { return lst(v)->text; }
IMPL static void Lbl_setFont(id v, SEL c, id f) { LabelSt *l = lst(v); objc_retain_(f); objc_release_(l->font); l->font = f; dirty(v); }
IMPL static id Lbl_font(id v, SEL c) { return lst(v)->font; }
IMPL static void Lbl_setColor(id v, SEL c, id k) { LabelSt *l = lst(v); objc_retain_(k); objc_release_(l->color); l->color = k; dirty(v); }
IMPL static id Lbl_color(id v, SEL c) { return lst(v)->color; }
IMPL static void Lbl_setShadow(id v, SEL c, id k) { LabelSt *l = lst(v); objc_retain_(k); objc_release_(l->shadow); l->shadow = k; dirty(v); }
IMPL static void Lbl_setShadowOff(id v, SEL c, CGSize o) { LabelSt *l = lst(v); l->shx = o.w; l->shy = o.h; dirty(v); }
IMPL static void Lbl_setAlign(id v, SEL c, int a) { lst(v)->align = a; dirty(v); }
IMPL static int Lbl_align(id v, SEL c) { return lst(v)->align; }
IMPL static void Lbl_setLines(id v, SEL c, int n) { lst(v)->lines = n; dirty(v); }
IMPL static int Lbl_lines(id v, SEL c) { return lst(v)->lines; }
IMPL static void Lbl_setFit(id v, SEL c, int f) { lst(v)->fit = f; dirty(v); }
IMPL static void Lbl_setMin(id v, SEL c, float m) { lst(v)->minsize = m; dirty(v); }
IMPL static void Lbl_sizeToFit(id v, SEL c) {
  LabelSt *l = lst(v); float w, h;
  font_measure(l->text ? nsstring_utf8(l->text) : "", font_size(l->font), 0, l->lines, &w, &h);
  ViewSt *s = vst(v); float x0 = s->center.x - s->bounds.w / 2, y0 = s->center.y - s->bounds.h / 2;
  s->bounds.w = w; s->bounds.h = h; s->center = (CGPoint){x0 + w / 2, y0 + h / 2}; dirty(v);
}
// ---- UIImageView ----
IMPL static void IV_setImage(id v, SEL c, id img) { ViewSt *s = vst(v); objc_retain_(img); objc_release_(s->image); s->image = img; dirty(v); }
IMPL static id IV_image(id v, SEL c) { return vst(v)->image; }
IMPL static id IV_initWithImage(id v, SEL c, id img) {
  int w = 0, h = 0; cg_uiimage_size(img, &w, &h);
  CGRect r = {0, 0, w, h}; v = send_initWithFrame(v, r); IV_setImage(v, c, img); return v;
}
// ---- NSString: measuring and drawing ----
IMPL static CGSize Str_sizeFont(id str, SEL c, id f) { CGSize r; font_measure(nsstring_utf8(str), font_size(f), 0, 0, &r.w, &r.h); return r; }
IMPL static CGSize Str_sizeConstrained(id str, SEL c, id f, CGSize max, int lbm) {
  CGSize r; font_measure(nsstring_utf8(str), font_size(f), max.w, 0, &r.w, &r.h); if (max.h > 0 && r.h > max.h) r.h = max.h; return r;
}
IMPL static CGSize Str_sizeForWidth(id str, SEL c, id f, float w, int lbm) { CGSize r; font_measure(nsstring_utf8(str), font_size(f), 0, 1, &r.w, &r.h); if (r.w > w) r.w = w; return r; }
static float g_text_dx;   // horizontal text offset (only while the History image is being built)
static float g_hist_maxw;
static float g_justify_w;  // width (-1 = measure only) long History lines are justified to (0 = not justified)
// Justification: spreads the leftover space over the line's spaces. Only lines with spaces that already
// fill >=80 % of the width (short ones, like paragraph ends, stay left-aligned).
static int draw_justified(const char *t, float x, float y, float sz, float W) {
  float w, h; font_measure(t, sz, 0, 0, &w, &h);
  int spaces = 0; for (const char *q = t; *q; q++) { if (*q == ' ') spaces++; if (*q == '\n') return 0; }
  if (!spaces || w < W * 0.8f || w >= W) return 0;
  float extra = (W - w) / spaces, sw, sh; font_measure(" ", sz, 0, 0, &sw, &sh);
  char word[512]; const char *q = t;
  while (*q) {
    int n = 0; while (q[n] && q[n] != ' ' && n < 511) n++;
    memcpy(word, q, n); word[n] = 0;
    if (n) { float ww, wh; cg_text(cg_current(), word, x, y, 0, sz, 0, 0, 0); font_measure(word, sz, 0, 0, &ww, &wh); x += ww; }
    q += n;
    while (*q == ' ') { x += sw + extra; q++; }
  }
  return 1;
}
IMPL static CGSize Str_drawAtPointFont(id str, SEL c, CGPoint p, id f) {
  CGSize r; const char *t = nsstring_utf8(str); float sz = font_size(f);
  font_measure(t, sz, 0, 0, &r.w, &r.h);
  if (g_justify_w < 0) { if (r.w > g_hist_maxw) g_hist_maxw = r.w; return r; }   // measuring pass
  if (!(g_justify_w > 0 && draw_justified(t, p.x + g_text_dx, p.y, sz, g_justify_w)))
    cg_text(cg_current(), t, p.x + g_text_dx, p.y, 0, sz, 0, 0, 0);
  return r;
}
// History (DSMenuHistWindow makeHistImage, game+0x1512a9): the game places each line at a fixed x designed for
// the iPhone font; with ours there was more margin on the right (≈22 px left vs ≈36 px right at 480x320).
// The text is shifted 7 px right while that image is built, to centre the block. Only affects History.
#define HIST_TEXT_DX 7.0f
static void hist_hook(Class_t *k);
void hist_hook_c(Class_t *k) { hist_hook(k); }
static IMP g_hist_orig;
IMPL static void Hist_makeImage(id self, SEL c) {
  // 1st pass: measure the longest line; 2nd: draw justified to that width (equal margins with the +7 px)
  g_hist_maxw = 0; g_justify_w = -1;
  ((IMPL void (*)(id, SEL))g_hist_orig)(self, c);
  g_text_dx = HIST_TEXT_DX; g_justify_w = g_hist_maxw;
  ((IMPL void (*)(id, SEL))g_hist_orig)(self, c);
  static int logged; if (!logged) { logged = 1; dslogv("History: justified to %.0f px", g_hist_maxw); }
  g_text_dx = 0; g_justify_w = 0;
}
static void hist_hook(Class_t *k) {
  static int done; if (done) return;
  IMP o = class_lookup(k, sel_registerName("makeHistImage"));
  if (!o || o == (IMP)Hist_makeImage) return;
  g_hist_orig = o; class_add(k, "makeHistImage", Hist_makeImage); done = 1;
  dslogv("History: text centred (+%.0f px) and justified", HIST_TEXT_DX);
}
IMPL static CGSize Str_drawAtPointWidth(id str, SEL c, CGPoint p, float w, id f, int lbm) {
  CGSize r; const char *t = nsstring_utf8(str); float sz = font_size(f);
  font_measure(t, sz, 0, 1, &r.w, &r.h); cg_text(cg_current(), t, p.x, p.y, 0, sz, 0, 0, 1); return r;
}
IMPL static CGSize Str_drawInRectFontAlign(id str, SEL c, CGRect rc, id f, int lbm, int align) {
  CGSize r; const char *t = nsstring_utf8(str); float sz = font_size(f);
  font_measure(t, sz, rc.w, 0, &r.w, &r.h); cg_text(cg_current(), t, rc.x, rc.y, rc.w, sz, align, rc.w, 0); return r;
}
IMPL static CGSize Str_drawInRectFont(id str, SEL c, CGRect rc, id f) { return Str_drawInRectFontAlign(str, c, rc, f, 0, 0); }
IMPL static CGSize Str_drawInRectFontLbm(id str, SEL c, CGRect rc, id f, int lbm) { return Str_drawInRectFontAlign(str, c, rc, f, lbm, 0); }
// ---- UIImage: draw in the current context ----
IMPL static void Img_drawAt(id img, SEL c, CGPoint p) { cg_image(cg_current(), img, p.x, p.y, 0, 0, 1); }
IMPL static void Img_drawAtBlend(id img, SEL c, CGPoint p, int mode, float a) { cg_image(cg_current(), img, p.x, p.y, 0, 0, a); }
IMPL static void Img_drawIn(id img, SEL c, CGRect r) { cg_image(cg_current(), img, r.x, r.y, r.w, r.h, 1); }
IMPL static void Img_drawInBlend(id img, SEL c, CGRect r, int mode, float a) { cg_image(cg_current(), img, r.x, r.y, r.w, r.h, a); }
IMPL static float Font_pointSize(id f, SEL c) { return font_size(f); }
IMPL static float Font_systemSize(Class_t *k, SEL c) { return 14.0f; }

// ---- rendering a view into its texture ----
typedef IMPL void (*DrawRectFn)(id, SEL, CGRect);
static int view_overrides_drawrect(id v) {
  IMP imp = class_lookup(v->isa, sel_registerName("drawRect:"));
  return imp && imp != (IMP)View_drawRectNop;
}
void gfx_upload_view(uint32_t *tex, const uint8_t *rgba, int w, int h);
// statistics: which classes are redrawn (to find views that redraw every frame)
static unsigned g_render_count, g_render_skipped, g_dl_skipped; static uint64_t g_dl_us;
void cg_rec_begin(void *ctx); uint32_t cg_rec_end(void *ctx); void cg_rec_play(void *ctx); void cg_rec_drop(void *ctx);
static struct { Class_t *k; unsigned n; uint64_t us; } g_rstat[16];
static void render_stat_t(id v, uint64_t us) {
  for (int i = 0; i < 16; i++) {
    if (g_rstat[i].k == v->isa) { g_rstat[i].n++; g_rstat[i].us += us; return; }
    if (!g_rstat[i].k) { g_rstat[i].k = v->isa; g_rstat[i].n = 1; g_rstat[i].us = us; return; }
  }
}
void ui_render_report(void) {   // called every 300 frames from gl.c
  char buf[512]; int o = 0;
  for (int i = 0; i < 16 && g_rstat[i].k; i++) o += snprintf(buf + o, sizeof(buf) - o, "%s x%u (%.1f ms c/u) ", class_name(g_rstat[i].k), g_rstat[i].n, g_rstat[i].us / 1000.0 / g_rstat[i].n);
  if (g_render_count || g_dl_skipped)
    dslogv("composition: %u views redrawn, %u reused (%u by identical draw list, %.2f ms each) in 300 frames: %s",
          g_render_count, g_render_skipped, g_dl_skipped, g_dl_skipped ? g_dl_us / 1000.0 / g_dl_skipped : 0.0, o ? buf : "");
  memset(g_rstat, 0, sizeof(g_rstat)); g_render_count = g_render_skipped = g_dl_skipped = 0; g_dl_us = 0;
}
static void view_render(id v) {
  ViewSt *s = vst(v);
  int w = (int)ceilf(s->bounds.w), h = (int)ceilf(s->bounds.h);
  s->dirty = 0;
  if (w <= 0 || h <= 0 || w > 1024 || h > 1024) { s->texw = 0; return; }
  int is_label = s->label != NULL, has_draw = view_overrides_drawrect(v), is_img = s->image != NULL;
  uint32_t bg = color_rgba(s->bg);
  if (!is_label && !has_draw && !is_img && !(bg & 0xff)) { s->texw = 0; return; }
  if (!has_draw) {   // labels and images: if nothing changed since the last draw, reuse the texture
    uint32_t hsh = 2166136261u;
    #define MIX(x) do { hsh ^= (uint32_t)(x); hsh *= 16777619u; } while (0)
    MIX(w); MIX(h); MIX(bg); MIX((uintptr_t)s->image);
    if (is_label) {
      LabelSt *l = s->label; const char *t = l->text ? nsstring_utf8(l->text) : "";
      for (const char *q = t; *q; q++) MIX((uint8_t)*q);
      MIX(color_rgba(l->color)); MIX(color_rgba(l->shadow)); MIX((int)(font_size(l->font) * 16)); MIX((int)(l->shx * 16)); MIX((int)(l->shy * 16));
      MIX(l->align); MIX(l->lines); MIX(l->fit); MIX((int)(l->minsize * 16));
    }
    #undef MIX
    if (s->texw && hsh == s->sig) { g_render_skipped++; return; }
    s->sig = hsh;
  }
  g_render_count++; uint64_t t_r0 = sceKernelGetProcessTimeWide();
  void *ctx = cg_new_uikit(w, h);
  int dw, dh, pitch; uint8_t *data = cg_data(ctx, &dw, &dh, &pitch);
  if (bg & 0xff) for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) { uint8_t *d = data + y * pitch + x * 4; d[0] = bg >> 24; d[1] = bg >> 16; d[2] = bg >> 8; d[3] = bg; }
  if (is_img) cg_image(ctx, s->image, 0, 0, w, h, 1);
  if (is_label && s->label->text) {
    LabelSt *l = s->label; const char *t = nsstring_utf8(l->text); float sz = font_size(l->font);
    int lines = l->lines; float maxw = lines == 1 ? 0 : (float)w;
    float tw, th; font_measure(t, sz, maxw, lines, &tw, &th);
    if (l->fit && tw > w && lines == 1) { float m = l->minsize > 0 ? l->minsize : sz * 0.5f; while (sz > m && tw > w) { sz -= 0.5f; font_measure(t, sz, 0, 1, &tw, &th); } }
    float y = (h - th) / 2;                     // UILabel centra verticalmente
    if (l->shadow && (color_rgba(l->shadow) & 0xff)) { cg_set_fill(ctx, color_rgba(l->shadow)); cg_text(ctx, t, l->shx, y + l->shy, w, sz, l->align, maxw, lines); }
    cg_set_fill(ctx, l->color ? color_rgba(l->color) : 0x000000ff);
    cg_text(ctx, t, 0, y, w, sz, l->align, maxw, lines);
  }
  if (has_draw) {
    cg_rec_begin(ctx);
    cg_push(ctx);
    SEL sel = sel_registerName("drawRect:"); CGRect r = {0, 0, w, h};
    ((DrawRectFn)msg_lookup(v, sel))(v, sel, r);
    cg_pop();
    // signature = draw list + size + background + own image/label
    uint32_t hsh = cg_rec_end(ctx) ^ (uint32_t)(w * 7919 + h) ^ bg ^ (uint32_t)(uintptr_t)s->image;
    if (is_label && s->label->text) { const char *t = nsstring_utf8(s->label->text); for (const char *q = t; *q; q++) hsh = (hsh ^ (uint8_t)*q) * 16777619u; }
    if (s->texw == w && s->texh == h && hsh == s->sig) {   // same content as the current texture
      cg_rec_drop(ctx); cg_free(ctx); g_render_skipped++; g_dl_skipped++;
      g_dl_us += sceKernelGetProcessTimeWide() - t_r0;
      return;
    }
    s->sig = hsh;
    cg_rec_play(ctx);
  }
  gfx_upload_view(&s->tex, data, w, h); s->texw = w; s->texh = h;
  cg_free(ctx);
  render_stat_t(v, sceKernelGetProcessTimeWide() - t_r0);
}
// ---- composition: walks EAGLView's subviews and draws them on top of the GL frame ----
void gfx_draw_view_tex(uint32_t tex, int w, int h, float x, float y, float alpha);
static id g_gl_view;
// Menu cursor hidden while the touch screen is used; it comes back when the controller is used (drawing only).
static int g_cursor_hidden;
void ui_cursor_set_hidden(int h) { g_cursor_hidden = h; }
int ui_touch_mode(void) { return g_cursor_hidden; }   // 1 = the touch screen is being used
static void compose(id v, float ox, float oy, float alpha) {
  ViewSt *s = vst(v);
  if (s->hidden || s->alpha <= 0.01f) return;
  float a = alpha * s->alpha;
  float x = ox + s->center.x - s->bounds.w / 2, y = oy + s->center.y - s->bounds.h / 2;
  if (g_cursor_hidden && v->isa == C_ImageViewK && (int)s->bounds.w == 32 && (int)s->bounds.h == 32 && s->superview == g_gl_view) return;
  if (v != g_gl_view) { x += s->t.tx; y += s->t.ty; }   // translation of the transform (the game centres labels this way)
  if (v != g_gl_view) {
    if (s->dirty) view_render(v);
    if (s->texw) gfx_draw_view_tex(s->tex, s->texw, s->texh, x, y, a);
  } else { x = 0; y = 0; }
  if (!s->subviews) return;
  uint32_t n = (uint32_t)MSG(s->subviews, "count");
  for (uint32_t i = 0; i < n; i++) compose(MSG(s->subviews, "objectAtIndex:", i), x - s->bounds.x, y - s->bounds.y, a);
}
// ---- touches: iOS-style hitTest inside EAGLView's subtree (same coordinates as the composition) ----
static id hit(id v, float px, float py) {     // (px,py) in v's bounds space
  ViewSt *s = vst(v);
  if (s->hidden || s->alpha <= 0.01f || !view_ui(v)) return NULL;
  if (px < s->bounds.x || py < s->bounds.y || px >= s->bounds.x + s->bounds.w || py >= s->bounds.y + s->bounds.h) return NULL;
  if (s->subviews) {
    uint32_t n = (uint32_t)MSG(s->subviews, "count");
    for (uint32_t i = n; i-- > 0;) {
      id sub = MSG(s->subviews, "objectAtIndex:", i); ViewSt *t = vst(sub);
      id r = hit(sub, px - (t->center.x - t->bounds.w / 2 + t->t.tx) + t->bounds.x, py - (t->center.y - t->bounds.h / 2 + t->t.ty) + t->bounds.y);
      if (r) return r;
    }
  }
  return v;
}
static id gl_view(void) { if (!g_gl_view) g_gl_view = view_find_responder("drawView:"); return g_gl_view; }
// Returns the view that receives the touch (the touched view or the first ancestor implementing touchesBegan:).
id ui_touch_target(float x, float y, id fallback) {
  id root = gl_view(); if (!root) return fallback;
  ViewSt *rs = vst(root);
  id v = hit(root, x + rs->bounds.x, y + rs->bounds.y);
  SEL sb = sel_registerName("touchesBegan:withEvent:");
  for (; v; v = vst(v)->superview) {
    IMP imp = class_lookup(v->isa, sb);
    if (imp && imp != (IMP)View_touchNop) break;
    if (v == root) { v = NULL; break; }
  }
  if (v && v != fallback) dslogv("touch -> %s", class_name(v->isa));
  return v ? v : fallback;
}
// Origin of view in game coordinates (EAGLView). 0 if it is not in that subtree.
int ui_view_origin(id view, float *ox, float *oy) {
  id root = gl_view(); float x = 0, y = 0;
  for (id v = view; v; v = vst(v)->superview) {
    if (v == root) { *ox = x - vst(root)->bounds.x; *oy = y - vst(root)->bounds.y; return 1; }
    ViewSt *s = vst(v); if (!s->superview) break;
    x += s->center.x - s->bounds.w / 2 + s->t.tx - s->bounds.x;
    y += s->center.y - s->bounds.h / 2 + s->t.ty - s->bounds.y;
  }
  return 0;
}
// ○ in menus: looks for a visible UIButton titled Cancel/No/Back and fires its action.
static int find_cancel(id v) {
  ViewSt *s = vst(v);
  if (s->hidden || s->alpha <= 0.01f) return 0;
  if (s->subviews) {
    uint32_t n = (uint32_t)MSG(s->subviews, "count");
    for (uint32_t i = n; i-- > 0;) if (find_cancel(MSG(s->subviews, "objectAtIndex:", i))) return 1;
  }
  if (v->isa == C_Button && s->label && s->label->text && s->target && s->action && s->ui != 2) {
    const char *t = nsstring_utf8(s->label->text);
    if (!strcasecmp(t, "cancel") || !strcasecmp(t, "no") || !strcasecmp(t, "back") || !strcasecmp(t, "close")) {
      dslogv("UIButton '%s' (○) -> %s %s", t, class_name(s->target->isa), s->action);
      objc_retain_(v); MSG(s->target, s->action, v); objc_release_(v);
      return 1;
    }
  }
  return 0;
}
// Is there a visible label whose text contains sub? (e.g. "want to quit" in the quit confirmation)
static int find_label(id v, const char *sub) {
  ViewSt *s = vst(v);
  if (s->hidden || s->alpha <= 0.01f) return 0;
  if (s->label && s->label->text) { const char *t = nsstring_utf8(s->label->text); if (t && strstr(t, sub)) return 1; }
  if (s->subviews) {
    uint32_t n = (uint32_t)MSG(s->subviews, "count");
    for (uint32_t i = 0; i < n; i++) if (find_label(MSG(s->subviews, "objectAtIndex:", i), sub)) return 1;
  }
  return 0;
}
int ui_label_visible(const char *sub) { id root = gl_view(); return root ? find_label(root, sub) : 0; }
int ui_is_button(id v) { for (Class_t *k = v ? v->isa : NULL; k; k = k->superclass) if (k == C_Button) return 1; return 0; }
int ui_press_cancel_button(void) { id root = gl_view(); return root ? find_cancel(root) : 0; }
// Is the game's cursor visible (32x32 UIImageView of mouse.png, child of EAGLView)? Menus only.
int ui_cursor_visible(void) {
  id root = gl_view(); if (!root || !vst(root)->subviews) return 0;
  id subs = vst(root)->subviews; uint32_t n = (uint32_t)MSG(subs, "count");
  for (uint32_t i = 0; i < n; i++) {
    id v = MSG(subs, "objectAtIndex:", i); ViewSt *t = vst(v);
    if (v->isa == C_ImageViewK && (int)t->bounds.w == 32 && (int)t->bounds.h == 32) return !t->hidden && t->alpha > 0.01f;
  }
  return 0;
}
void ui_compose(void) {
  if (!g_gl_view) { g_gl_view = view_find_responder("drawView:"); if (!g_gl_view) return; }
  compose(g_gl_view, 0, 0, 1);
}

// ---- UIButton (DSUIOrderWindow makeButton:text:tag: → buttonWithType:1, setTitle:forState:, addTarget:...) ----
// iOS 7 "system" style: title only, in blue; dimmed while pressed. The action fires on release.
// C_Button declared above
static id send_initWithFrame(id v, CGRect r);
IMPL static id Btn_withType(Class_t *cl, SEL c, int type) {
  CGRect z = {0, 0, 0, 0};
  id b = send_initWithFrame(MSG((id)cl, "alloc"), z);
  LabelSt *l = lst(b); l->align = 1;
  Font *f = (Font *)objc_alloc(C_Font, 0); f->size = 15; l->font = (id)f;
  Color *k = (Color *)objc_alloc(C_Color, 0); k->r = 0; k->g = 122 / 255.0f; k->b = 1; k->a = 1;
  l->color = type == 0 ? MSG((id)C_Color, "whiteColor") : (id)k;
  if (type == 0) { objc_retain_(l->color); objc_release_((id)k); }
  vst(b)->ui = 1;
  dslogv("UIButton buttonWithType:%d", type);
  return objc_autorelease_(b);
}
IMPL static void Btn_setTitle(id b, SEL c, id t, int state) { if (state == 0) Lbl_setText(b, c, t); }
IMPL static id Btn_title(id b, SEL c, int state) { return lst(b)->text; }
IMPL static void Btn_setTitleColor(id b, SEL c, id k, int state) { if (state == 0) Lbl_setColor(b, c, k); }
IMPL static id Btn_titleLabel(id b, SEL c) { return b; }   // UILabel calls (setFont:, etc.) go to the button itself
IMPL static void Btn_setImage(id b, SEL c, id img, int state) { if (state == 0) IV_setImage(b, c, img); }
IMPL static void Btn_addTarget(id b, SEL c, id target, SEL action, unsigned events) {
  ViewSt *s = vst(b);
  if (events & ((1u << 6) | (1u << 0))) { s->target = target; s->action = action; }   // TouchUpInside / TouchDown
  dslogv("UIButton addTarget:%s action:%s eventos=%#x", target ? class_name(target->isa) : "nil", action ? action : "-", events);
}
IMPL static void Btn_began(id b, SEL c, id t, id e) { ViewSt *s = vst(b); s->pressed = 1; s->alpha = 0.4f; }
IMPL static void Btn_moved(id b, SEL c, id t, id e) {}
IMPL static void Btn_ended(id b, SEL c, id t, id e) {
  ViewSt *s = vst(b); s->alpha = 1;
  if (s->pressed && s->target && s->action) {
    dslogv("UIButton '%s' -> %s %s", s->label && s->label->text ? nsstring_utf8(s->label->text) : "", class_name(s->target->isa), s->action);
    objc_retain_(b);
    MSG(s->target, s->action, b);
    objc_release_(b);
  }
  s->pressed = 0;
}
IMPL static void Btn_cancelled(id b, SEL c, id t, id e) { ViewSt *s = vst(b); s->pressed = 0; s->alpha = 1; }
IMPL static void Btn_setEnabled(id b, SEL c, int en) { vst(b)->ui = en ? 1 : 2; }
IMPL static int Btn_enabled(id b, SEL c) { return vst(b)->ui != 2; }
static void button_init(Class_t *B) {
  C_Button = B;
  class_add_meta(B, "buttonWithType:", Btn_withType);
  class_add(B, "setTitle:forState:", Btn_setTitle); class_add(B, "titleForState:", Btn_title);
  class_add(B, "setTitleColor:forState:", Btn_setTitleColor); class_add(B, "titleLabel", Btn_titleLabel);
  class_add(B, "setImage:forState:", Btn_setImage); class_add(B, "setBackgroundImage:forState:", Btn_setImage);
  class_add(B, "addTarget:action:forControlEvents:", Btn_addTarget);
  class_add(B, "touchesBegan:withEvent:", Btn_began); class_add(B, "touchesMoved:withEvent:", Btn_moved);
  class_add(B, "touchesEnded:withEvent:", Btn_ended); class_add(B, "touchesCancelled:withEvent:", Btn_cancelled);
  class_add(B, "setEnabled:", Btn_setEnabled); class_add(B, "isEnabled", Btn_enabled);
  class_add(B, "setFont:", Lbl_setFont); class_add(B, "setTextColor:", Lbl_setColor); class_add(B, "setShadowColor:", Lbl_setShadow);
  class_add(B, "setTextAlignment:", Lbl_setAlign);
}
void uikit_text_init(Class_t *LBL) {
  font_init();
  class_add(C_Color, "set", Color_set); class_add(C_Color, "setFill", Color_set); class_add(C_Color, "setStroke", Nop);
  class_add(C_View, "setNeedsDisplay", View_setNeedsDisplay); class_add(C_View, "setNeedsDisplayInRect:", View_setNeedsDisplayRect);
  class_add(C_View, "setBackgroundColor:", View_setBg); class_add(C_View, "backgroundColor", View_bg);
  class_add(C_View, "drawRect:", View_drawRectNop);
  class_add(LBL, "setText:", Lbl_setText); class_add(LBL, "text", Lbl_text);
  class_add(LBL, "setFont:", Lbl_setFont); class_add(LBL, "font", Lbl_font);
  class_add(LBL, "setTextColor:", Lbl_setColor); class_add(LBL, "textColor", Lbl_color);
  class_add(LBL, "setShadowColor:", Lbl_setShadow); class_add(LBL, "setShadowOffset:", Lbl_setShadowOff);
  class_add(LBL, "setTextAlignment:", Lbl_setAlign); class_add(LBL, "textAlignment", Lbl_align);
  class_add(LBL, "setNumberOfLines:", Lbl_setLines); class_add(LBL, "numberOfLines", Lbl_lines);
  class_add(LBL, "setAdjustsFontSizeToFitWidth:", Lbl_setFit); class_add(LBL, "setMinimumFontSize:", Lbl_setMin);
  class_add(LBL, "sizeToFit", Lbl_sizeToFit);
  Class_t *IV = objc_getClass("UIImageView");
  class_add(IV, "setImage:", IV_setImage); class_add(IV, "image", IV_image); class_add(IV, "initWithImage:", IV_initWithImage);
  Class_t *S = g_cls_NSString;
  class_add(S, "sizeWithFont:", Str_sizeFont); class_add(S, "sizeWithFont:constrainedToSize:lineBreakMode:", Str_sizeConstrained);
  class_add(S, "sizeWithFont:constrainedToSize:", Str_sizeConstrained); class_add(S, "sizeWithFont:forWidth:lineBreakMode:", Str_sizeForWidth);
  class_add(S, "drawAtPoint:withFont:", Str_drawAtPointFont);
  class_add(S, "drawAtPoint:forWidth:withFont:lineBreakMode:", Str_drawAtPointWidth);
  class_add(S, "drawInRect:withFont:", Str_drawInRectFont); class_add(S, "drawInRect:withFont:lineBreakMode:", Str_drawInRectFontLbm);
  class_add(S, "drawInRect:withFont:lineBreakMode:alignment:", Str_drawInRectFontAlign);
  Class_t *IMG = objc_getClass("UIImage");
  class_add(IMG, "drawAtPoint:", Img_drawAt); class_add(IMG, "drawAtPoint:blendMode:alpha:", Img_drawAtBlend);
  class_add(IMG, "drawInRect:", Img_drawIn); class_add(IMG, "drawInRect:blendMode:alpha:", Img_drawInBlend);
  class_add(C_Font, "pointSize", Font_pointSize); class_add_meta(C_Font, "systemFontSize", Font_systemSize);
}

// ---- UIAlertView: no UI of its own yet; logged and closed with the cancel button ----
typedef struct { Class_t *isa; ViewSt *st; id title, msg, delegate; int nbuttons; } Alert;
IMPL static id Alert_init(Alert *a, SEL c, id title, id msg, id del, id cancel, id other, ...) {
  a->title = objc_retain_(title); a->msg = objc_retain_(msg); a->delegate = del; a->nbuttons = (cancel ? 1 : 0) + (other ? 1 : 0);
  return (id)a;
}
static void alert_dismiss(id a_) {
  Alert *a = (Alert *)a_;
  if (a->delegate && class_responds(a->delegate->isa, sel_registerName("alertView:clickedButtonAtIndex:"))) MSG(a->delegate, "alertView:clickedButtonAtIndex:", (id)a, 0);
  if (a->delegate && class_responds(a->delegate->isa, sel_registerName("alertView:didDismissWithButtonIndex:"))) MSG(a->delegate, "alertView:didDismissWithButtonIndex:", (id)a, 0);
}
IMPL static void Alert_show(Alert *a, SEL c) {
  dslog("[warning] UIAlertView \"%s\": \"%s\" -> closed with button 0", a->title ? nsstring_utf8(a->title) : "", a->msg ? nsstring_utf8(a->msg) : "");
  objc_retain_((id)a); runloop_post((id)a, sel_registerName("__alertDismiss"), NULL);
}
IMPL static void Alert_dismissNow(Alert *a, SEL c, id unused) { alert_dismiss((id)a); objc_release_((id)a); }
IMPL static int Alert_cancelIdx(Alert *a, SEL c) { return 0; }
IMPL static int Alert_nbuttons(Alert *a, SEL c) { return a->nbuttons; }
IMPL static void Alert_setDelegate(Alert *a, SEL c, id d) { a->delegate = d; }
void uikit_alert_init(void) {
  Class_t *A = objc_getClass("UIAlertView");
  if (!A) A = objc_class_new("UIAlertView", C_View, sizeof(Alert));
  class_add(A, "initWithTitle:message:delegate:cancelButtonTitle:otherButtonTitles:", Alert_init);
  class_add(A, "show", Alert_show); class_add(A, "__alertDismiss", Alert_dismissNow);
  class_add(A, "cancelButtonIndex", Alert_cancelIdx); class_add(A, "numberOfButtons", Alert_nbuttons);
  class_add(A, "setDelegate:", Alert_setDelegate); class_add(A, "dismissWithClickedButtonIndex:animated:", Nop);
}
