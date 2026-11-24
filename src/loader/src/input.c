// Input: front touch screen -> UITouch/UIEvent; Vita buttons -> MFi game controller (GCController).
// Also: L+R+SELECT = PNG screenshot of the frame (ux0:data/deathsmiles/shots/).
#include <stdlib.h>
#include <string.h>
#include <psp2/touch.h>
#include <psp2/ctrl.h>
#include <psp2/io/fcntl.h>
#include <stdio.h>
#include "objc.h"
#include "gfx.h"

typedef struct { float x, y; } CGPoint;

// ---------------- NSSet (minimal) ----------------
typedef struct { Class_t *isa; uint32_t n; id v[8]; } Set;
static Class_t *C_Set, *C_Touch, *C_Event;
static Set *set_new(void) { return (Set *)objc_alloc(C_Set, 0); }
// +[NSMutableSet set] / addObject: (StoreKit uses them for the product list; capacity 8, the rest is ignored)
IMPL static id Set_newAuto(Class_t *cl, SEL c) { return objc_autorelease_((id)set_new()); }
IMPL static id Set_any(Set *s, SEL c) { return s->n ? s->v[0] : NULL; }
IMPL static uint32_t Set_count(Set *s, SEL c) { return s->n; }
IMPL static id Set_all(Set *s, SEL c) {
  id a = MSG((id)objc_getClass("NSMutableArray"), "array");
  for (uint32_t i = 0; i < s->n; i++) MSG(a, "addObject:", s->v[i]);
  return a;
}
IMPL static int Set_contains(Set *s, SEL c, id o) { for (uint32_t i = 0; i < s->n; i++) if (s->v[i] == o) return 1; return 0; }
typedef struct { uint32_t state; id *items; uint32_t *mut; uint32_t extra[5]; } FastEnum;
static uint32_t g_mut;
IMPL static uint32_t Set_enum(Set *s, SEL c, FastEnum *st, id *buf, uint32_t len) {
  if (st->state) return 0; st->state = 1; st->items = s->v; st->mut = &g_mut; return s->n;
}
IMPL static void Set_dealloc(Set *s, SEL c) { for (uint32_t i = 0; i < s->n; i++) objc_release_(s->v[i]); objc_free_object((id)s); }
static void set_add(Set *s, id o) { if (s->n < 8) s->v[s->n++] = objc_retain_(o); }
IMPL static void Set_addObj(Set *s, SEL c, id o) { set_add(s, o); }

// ---------------- UITouch / UIEvent ----------------
typedef struct { Class_t *isa; int phase, taps; float x, y, px, py; double ts; id view; } Touch;
enum { PH_BEGAN, PH_MOVED, PH_STATIONARY, PH_ENDED, PH_CANCELLED };
// Coordinates of the game view (480x320, origin top-left). The window is portrait
// 320x480 and the view rotated -90°: window = (gy, 480 - gx).
int ui_view_origin(id view, float *ox, float *oy);
id ui_touch_target(float x, float y, id fallback);
static CGPoint loc_in(float x, float y, id view) {
  float ox, oy;
  if (view && ui_view_origin(view, &ox, &oy)) { CGPoint p = {x - ox, y - oy}; return p; }
  CGPoint w = {y, 480 - x}; return w;      // ventana / nil
}
IMPL static CGPoint Touch_loc(Touch *t, SEL c, id view) { return loc_in(t->x, t->y, view); }
IMPL static CGPoint Touch_prev(Touch *t, SEL c, id view) { return loc_in(t->px, t->py, view); }
IMPL static int Touch_phase(Touch *t, SEL c) { return t->phase; }
IMPL static int Touch_taps(Touch *t, SEL c) { return t->taps; }
IMPL static double Touch_ts(Touch *t, SEL c) { return t->ts; }
IMPL static id Touch_view(Touch *t, SEL c) { return t->view; }
typedef struct { Class_t *isa; id all; } Event;
IMPL static id Event_all(Event *e, SEL c) { return e->all; }
IMPL static id Event_forView(Event *e, SEL c, id v) { return e->all; }

#define MAXT 6
static struct { int active, id; Touch *t; } g_tp[MAXT];
static id g_touch_view;
id view_find_responder(const char *sel);   // uikit.c: first view (in the window) that implements sel
float gfx_scale(void); int gfx_ox(void), gfx_oy(void);
double cf_now(void);

// ---- Power Up on a physical button ----
// The game (iOS 7) maps no MFi controller button to Power Up: it only exists as an on-screen button
// (touch button no. 5, bit 0x200 in the input mask of game+0xde54). R simulates a short tap
// at the centre of that button. The game's position table (0x51c9ec) gave (544,384), which does not match
// the 480x320 screen, so the position measured on screenshots is used: centre (343,281).
// In Smartphone mode that same button is LOCK SHOT. With the menu cursor visible, R does nothing.
int ui_cursor_visible(void);
void ui_cursor_set_hidden(int h);
static uint32_t g_buttons;
// ---- Control types ----
// 0 = Type iOS (the game's MFi mapping as is; R = Power Up / Lock Shot).
// 1 = Type 360. Finding (disassembly of game+0xd4a0): the game has two controller modes depending on the
// "buttonAutoShot" option (u16 at game 0x5feeac, copied from the save object at startup):
//   Auto Shot ON : automatic fire; X = Lock Shot (0x400), A = change direction (0x800); L/R ignored.
//   Auto Shot OFF: X = left shot (0x10), A = right shot (0x20), X+A or Y = laser (0x40),
//                  L/R = left/right turbo (a pulse every 4 frames). This is the Xbox 360 layout.
// Type 360 forces Auto Shot OFF while active (in memory only; the save data is untouched) and switching back
// to Type iOS restores the original value. △ = Lock Shot / Power Up (synthetic tap), R is no longer Power Up.
// Three layouts. 1 = Type 360, matching the options menu of the Xbox 360 game:
// A = Left Shot, B = Right Shot, X = Lock Shot, Y = Bomb, LT/RT = Turbo. On the Vita, by position:
// × = left, ○ = right, □ = Lock Shot/Power Up, △ = bomb, L/R = turbo. 2 = Type Custom: an alternative layout
// (□ left, × right, △ Lock Shot/Power Up, ○ bomb, L/R turbo), kept as an option.
// SELECT + L cycles iOS -> 360 -> Custom -> iOS.
enum { SCH_IOS, SCH_360, SCH_CUSTOM, SCH_N };
static const char *const g_sch_name[SCH_N] = {"iOS", "360", "Custom"};
static const char *const g_sch_file[SCH_N] = {"ios", "360", "custom"};
static int g_scheme;          // SCH_*
static int g_in_play;         // game in progress without the menu cursor (computed once per frame)
static int g_autoshot_saved = -1;
#define AUTOSHOT_ADDR 0x5feeac
#define SCHEME_FILE "ux0:data/deathsmiles/controls.txt"
#define SCHEME_FILE_OLD "ux0:data/deathsmiles/controles.txt"   // old file name (migrated and deleted)
static volatile uint16_t *autoshot_ptr(void) { return (volatile uint16_t *)(g_base + AUTOSHOT_ADDR - g_vmbase); }
static void scheme_save(void) {
  SceUID fd = sceIoOpen(SCHEME_FILE, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
  if (fd >= 0) { char t[16]; int n = snprintf(t, sizeof t, "%s\n", g_sch_file[g_scheme]); sceIoWrite(fd, t, n); sceIoClose(fd); }
}
static void scheme_load(void) {
  char b[8] = {0}; SceUID fd = sceIoOpen(SCHEME_FILE, SCE_O_RDONLY, 0);
  int migrate = 0;
  if (fd < 0) { fd = sceIoOpen(SCHEME_FILE_OLD, SCE_O_RDONLY, 0); migrate = fd >= 0; }
  if (fd >= 0) { sceIoRead(fd, b, sizeof b - 1); sceIoClose(fd); }
  g_scheme = SCH_IOS;
  for (int i = 1; i < SCH_N; i++) if (!strncmp(b, g_sch_file[i], strlen(g_sch_file[i]))) g_scheme = i;
  if (migrate) { scheme_save(); sceIoRemove(SCHEME_FILE_OLD); }
  dslogv("controles: Type %s", g_sch_name[g_scheme]);
}
// Every frame: in Type 360 keeps Auto Shot OFF (the game copies the option again when leaving Options).
static void scheme_autoshot(void) {
  volatile uint16_t *a = autoshot_ptr();
  if (g_scheme) {
    if (g_autoshot_saved < 0 && !g_in_play) return;   // wait for gameplay: the option is loaded by then
    if (g_autoshot_saved < 0) { g_autoshot_saved = *a; dslogv("360 controls: game Auto Shot = %d -> forced OFF", g_autoshot_saved); }
    if (*a) *a = 0;
  } else if (g_autoshot_saved >= 0) {
    *a = (uint16_t)g_autoshot_saved; dslogv("controles iOS: Auto Shot restaurado a %d", g_autoshot_saved); g_autoshot_saved = -1;
  }
}
// State the game sees for each MFi profile button, according to the layout.
static uint32_t scheme_mask(uint32_t mask, uint32_t phys) {
  if (!g_scheme) return mask == SCE_CTRL_RTRIGGER ? 0 : phys & mask;   // iOS: R is Power Up, not the shoulder button
  if (!g_in_play) return mask == SCE_CTRL_RTRIGGER ? 0 : phys & mask;   // menus: same as iOS
  // controller Y = bit 0x40 = Lock Shot / Power Up (index 10 = PAUSE, and the synthetic tap fires).
  if (g_scheme == SCH_CUSTOM) return phys & mask;            // MFi profile as is (alternative layout)
  // Type 360: MFi profile button the game asks for -> physical Vita button
  switch (mask) {
    case SCE_CTRL_SQUARE:   return phys & SCE_CTRL_CROSS;     // X (left shot)  <- ×
    case SCE_CTRL_CROSS:    return phys & SCE_CTRL_CIRCLE;    // A (right shot)  <- ○
    case SCE_CTRL_TRIANGLE: return phys & SCE_CTRL_SQUARE;    // Y (Lock Shot/Power Up) ← □
    case SCE_CTRL_CIRCLE:   return phys & SCE_CTRL_TRIANGLE;  // B (bomba)          ← △
    default:                return phys & mask;               // L/R (turbo), D-pad
  }
}
// Power Up is activated by HOLDING the button (tutorial "Hold down the POWER UP button"): the synthetic tap
// lasts while R is held. 0 = inactive, 1 = just pressed (menu check pending), 2 = touching.
static int g_pu_frames;
static int powerup_touch(float *x, float *y) {
  if (!g_pu_frames) return 0;
  if (!(g_buttons & SCE_CTRL_RTRIGGER)) { g_pu_frames = 0; return 0; }   // button released -> the tap ends
  if (g_pu_frames == 1) {
    if (ui_cursor_visible()) { g_pu_frames = 0; dslogv("power up: R ignored (menu cursor visible)"); return 0; }
    g_pu_frames = 2;
  }
  *x = 343; *y = 281;
  return 1;
}
// ---- Menus with the controller: × on system buttons and ○ = Cancel/Back ----
// The game's cursor (UIImageView at game 0x522b74) has its tip at (x,y) = integers at game 0x522b64/0x522b68,
// 480x320 coordinates; the game itself detects clicks with A only on its drawn areas. For UIKit elements
// (the shop's OK/CANCEL UIButtons, etc.) a short tap at the cursor tip is simulated.
// ○ in menus: if a "Cancel"/"No" UIButton is visible it is pressed; otherwise the Back icon (top right) is tapped.
id ui_touch_target(float x, float y, id fallback);
int ui_press_cancel_button(void);
static int g_syn_frames; static float g_syn_x, g_syn_y;
static uint32_t g_swallow;      // buttons the game must not see until they are released
#define BACK_ICON_X 462
#define BACK_ICON_Y 18
#define PAUSE_ICON_X 16
#define PAUSE_ICON_Y 16
// Pause menu: RESUME (113,219), QUIT (367,219); confirmation "Are you sure you want to quit?":
// Yes (113,216), No (369,216). In pause ○ or START = RESUME; in the confirmation ○ = No.
#define RESUME_X 113
#define RESUME_Y 219
#define QUIT_NO_X 369
#define QUIT_NO_Y 216
#define QUIT_X 367
#define QUIT_Y 219
#define QUIT_YES_X 113
#define QUIT_YES_Y 216
int ui_label_visible(const char *sub);
static int g_pause_state, g_pause_wait;
// Quit confirmation: Arcade "Are you sure you want to quit?"; Smartphone "Any treasure items you have
// collected in this stage will be lost." (it was not detected at first and × tapped QUIT again = the No position).
static int quit_confirm_visible(void) { return ui_label_visible("want to quit") || ui_label_visible("will be lost"); }
// "Continue?" screen: Yes (113,218), No (369,218). Detected as "the menu cursor appears
// during gameplay without a pause request". ○ = No, only once (then ○ goes back to normal), so that
// position is not tapped on other screens shown after the game (results, stage map).
#define CONT_NO_X 369
#define CONT_NO_Y 218
#define CONT_YES_X 113
#define CONT_YES_Y 218
static int g_play_frames, g_cont_state;   // 0 playing, 1 pause requested, 2 pause menu on screen
static void syn_tap(float x, float y, const char *why) {
  g_syn_x = x; g_syn_y = y; g_syn_frames = 3;
  dslogv("controller: %s -> tap at (%d,%d)", why, (int)x, (int)y);
}
static void cursor_pos(float *x, float *y) {
  int32_t *p = (int32_t *)(g_base + 0x522b64 - g_vmbase);
  *x = p[0] + 1; *y = p[1] + 1;
}
static void menu_buttons(uint32_t pressed) {
  if (!ui_cursor_visible()) return;                      // only in menus (during gameplay the cursor is hidden)
  if (pressed & SCE_CTRL_CROSS) {
    float x, y; cursor_pos(&x, &y);
    id t = ui_touch_target(x, y, NULL);
    extern int ui_is_button(id v);
    // System buttons only (UIButton): other views with their own touches, such as messageLabelView (480x320,
    // transparent, on top of everything while notices are shown), must not take the × the game's cursor needs.
    if (t && t != g_touch_view && ui_is_button(t)) { syn_tap(x, y, "× on UIKit button"); g_swallow |= SCE_CTRL_CROSS; }
  }
  if (g_pause_state == 2 && (pressed & SCE_CTRL_CROSS) && quit_confirm_visible()) {   // × = Yes (salir)
    g_swallow |= SCE_CTRL_CROSS; g_pause_state = 0; syn_tap(QUIT_YES_X, QUIT_YES_Y, "× = Yes (salir)"); return;
  }
  // Yes/No dialogs with a label ("Exit to the previous menu?" from the stage map/Practice):
  // × = Yes, ○ = No. Positions in 480x320.
  if (g_pause_state != 2 && ui_label_visible("Exit to the previous menu")) {
    if (pressed & SCE_CTRL_CROSS) { g_swallow |= SCE_CTRL_CROSS; syn_tap(118, 198, "× = Yes (exit to the previous menu)"); return; }
    if (pressed & SCE_CTRL_CIRCLE) { g_swallow |= SCE_CTRL_CIRCLE; syn_tap(360, 198, "○ = No (exit to the previous menu)"); return; }
  }
  if (g_pause_state == 2 && (pressed & SCE_CTRL_CROSS) && !quit_confirm_visible()) {   // × = QUIT (pause menu)
    g_swallow |= SCE_CTRL_CROSS; syn_tap(QUIT_X, QUIT_Y, "× = Quit (pausa)"); return;
  }
  if (g_pause_state == 2 && (pressed & (SCE_CTRL_CIRCLE | SCE_CTRL_START))) {
    g_swallow |= pressed & (SCE_CTRL_CIRCLE | SCE_CTRL_START);
    if (quit_confirm_visible()) syn_tap(QUIT_NO_X, QUIT_NO_Y, "○ = No (salir)");
    else syn_tap(RESUME_X, RESUME_Y, "○/Start = Resume");
    return;
  }
  if (g_cont_state && (pressed & SCE_CTRL_START)) {   // START = Yes (keep playing); × follows the game's cursor
    g_swallow |= SCE_CTRL_START; g_cont_state = 0;
    syn_tap(CONT_YES_X, CONT_YES_Y, "Start = Yes (Continue)"); return;
  }
  if (g_cont_state && (pressed & SCE_CTRL_CIRCLE)) {
    g_swallow |= SCE_CTRL_CIRCLE; g_cont_state = 0;
    syn_tap(CONT_NO_X, CONT_NO_Y, "○ = No (Continue)"); return;
  }
  if (pressed & SCE_CTRL_CIRCLE) {
    g_swallow |= SCE_CTRL_CIRCLE;
    if (ui_press_cancel_button()) dslogv("controller: ○ -> Cancel button");
    else syn_tap(BACK_ICON_X, BACK_ICON_Y, "○ = Volver");
  }
}
static int syn_touch(float *x, float *y) {
  if (g_syn_frames <= 0) return 0;
  if (--g_syn_frames == 0) return 0;                     // the last frame without a report = touchesEnded
  *x = g_syn_x; *y = g_syn_y; return 1;
}
// Delivery grouped by target view (each touch keeps its view from touchesBegan, as on iOS).
static void send_touches(const char *sel, Set *set) {
  if (!g_touch_view) return;
  int done[8] = {0};
  for (int i = 0; i < set->n; i++) {
    if (done[i]) continue;
    id target = ((Touch *)set->v[i])->view;
    Set *sub = set_new();
    for (int j = i; j < set->n; j++) if (!done[j] && ((Touch *)set->v[j])->view == target) { set_add(sub, set->v[j]); done[j] = 1; }
    Event *e = (Event *)objc_alloc(C_Event, 0); e->all = (id)sub;
    MSG(target, sel, (id)sub, (id)e);
    objc_free_object((id)e); objc_release_((id)sub);
  }
}
void input_poll_touch(void) {
  static int init;
  if (!init) { sceTouchSetSamplingState(SCE_TOUCH_PORT_FRONT, SCE_TOUCH_SAMPLING_STATE_START); init = 1; }
  if (!g_touch_view) g_touch_view = view_find_responder("touchesBegan:withEvent:");
  SceTouchData td; if (sceTouchPeek(SCE_TOUCH_PORT_FRONT, &td, 1) < 1) return;
  float s = gfx_scale(); int ox = gfx_ox(), oy = gfx_oy();
  Set *began = set_new(), *moved = set_new(), *ended = set_new();
  int seen[MAXT] = {0};
  int nrep = 0, rid[MAXT + 1]; float rx[MAXT + 1], ry[MAXT + 1];
  for (int r = 0; r < td.reportNum && r < MAXT; r++) {
    rid[nrep] = td.report[r].id;
    rx[nrep] = (td.report[r].x / 2.0f - ox) / s; ry[nrep] = (td.report[r].y / 2.0f - oy) / s;   // panel 1920x1088 -> 960x544 -> game
    { extern void gfx_skip_remap(float *, float *); gfx_skip_remap(&rx[nrep], &ry[nrep]); }   // moved SKIP button
    nrep++;
  }
  float px, py;
  if (powerup_touch(&px, &py) && nrep < MAXT) { rid[nrep] = 0x10000; rx[nrep] = px; ry[nrep] = py; nrep++; }   // synthetic tap
  if (syn_touch(&px, &py) && nrep < MAXT) { rid[nrep] = 0x10001; rx[nrep] = px; ry[nrep] = py; nrep++; }
  for (int r = 0; r < nrep; r++) {
    float gx = rx[r], gy = ry[r];
    int slot = -1;
    for (int k = 0; k < MAXT; k++) if (g_tp[k].active && g_tp[k].id == rid[r]) slot = k;
    if (slot < 0) {
      for (int k = 0; k < MAXT; k++) if (!g_tp[k].active) { slot = k; break; }
      if (slot < 0) continue;
      Touch *t = (Touch *)objc_alloc(C_Touch, 0);
      if (rid[r] < 0x10000) ui_cursor_set_hidden(1);
      if (rid[r] < 0x10000 && gx < 40 && gy < 40 && g_pause_state == 0 && !ui_cursor_visible()) { g_pause_state = 1; g_pause_wait = 0; }   // pause by tapping the icon   // real touch: hide the controller cursor
      { static int nlog; if (nlog < 40 && rid[r] < 0x10000) { nlog++; dslogv("touch in game (%d,%d)", (int)gx, (int)gy); } }
      t->phase = PH_BEGAN; t->taps = 1; t->x = t->px = gx; t->y = t->py = gy; t->ts = cf_now(); t->view = objc_retain_(ui_touch_target(gx, gy, g_touch_view));   // retained while the touch lasts
      g_tp[slot].active = 1; g_tp[slot].id = rid[r]; g_tp[slot].t = t;
      set_add(began, (id)t);
    } else {
      Touch *t = g_tp[slot].t;
      t->px = t->x; t->py = t->y;
      if (gx != t->x || gy != t->y) { t->x = gx; t->y = gy; t->phase = PH_MOVED; t->ts = cf_now(); set_add(moved, (id)t); }
      else t->phase = PH_STATIONARY;
    }
    seen[slot] = 1;
  }
  for (int k = 0; k < MAXT; k++) if (g_tp[k].active && !seen[k]) {
    Touch *t = g_tp[k].t; t->phase = PH_ENDED; t->ts = cf_now(); set_add(ended, (id)t);
    g_tp[k].active = 0; objc_release_((id)t);
  }
  if (began->n) send_touches("touchesBegan:withEvent:", began);
  if (moved->n) send_touches("touchesMoved:withEvent:", moved);
  if (ended->n) {
    send_touches("touchesEnded:withEvent:", ended);
    for (uint32_t i = 0; i < ended->n; i++) { Touch *t = (Touch *)ended->v[i]; objc_release_(t->view); t->view = NULL; }
  }
  objc_release_((id)began); objc_release_((id)moved); objc_release_((id)ended);
}

// ---------------- GCController (iOS 7 "gamepad" profile) ----------------
typedef struct { Class_t *isa; uint32_t mask; int axis; } Btn;      // axis: 0 button, 1 X axis, 2 Y axis
typedef struct { Class_t *isa; Btn *up, *down, *left, *right, *x, *y; } DPad;
typedef struct { Class_t *isa; Btn *a, *b, *x, *y, *l, *r; DPad *dpad; } Pad;
typedef struct { Class_t *isa; Pad *pad; void *paused_block; int index; } Ctl;
static int g_lx, g_ly;     // current Vita state
static Ctl *g_ctl; static id g_ctl_array;
static Class_t *C_Btn, *C_DPad, *C_Pad, *C_Ctl;
static Btn *btn(uint32_t mask, int axis) { Btn *b = (Btn *)objc_alloc(C_Btn, 0); b->mask = mask; b->axis = axis; return b; }
#define STICK_DEAD 40
static int dir_pressed(uint32_t mask) {
  int st = 0;
  if (mask == SCE_CTRL_UP) st = g_ly < 128 - STICK_DEAD; else if (mask == SCE_CTRL_DOWN) st = g_ly > 128 + STICK_DEAD;
  else if (mask == SCE_CTRL_LEFT) st = g_lx < 128 - STICK_DEAD; else if (mask == SCE_CTRL_RIGHT) st = g_lx > 128 + STICK_DEAD;
  return scheme_mask(mask, g_buttons & ~g_swallow) || st;
}
IMPL static int Btn_pressed(Btn *b, SEL c) { return b->axis ? 0 : dir_pressed(b->mask); }
IMPL static float Btn_value(Btn *b, SEL c) {
  if (b->axis == 1) return dir_pressed(SCE_CTRL_RIGHT) ? 1.0f : dir_pressed(SCE_CTRL_LEFT) ? -1.0f : 0.0f;
  if (b->axis == 2) return dir_pressed(SCE_CTRL_UP) ? 1.0f : dir_pressed(SCE_CTRL_DOWN) ? -1.0f : 0.0f;
  return dir_pressed(b->mask) ? 1.0f : 0.0f;
}
IMPL static id DPad_up(DPad *d, SEL c) { return (id)d->up; }
IMPL static id DPad_down(DPad *d, SEL c) { return (id)d->down; }
IMPL static id DPad_left(DPad *d, SEL c) { return (id)d->left; }
IMPL static id DPad_right(DPad *d, SEL c) { return (id)d->right; }
IMPL static id DPad_x(DPad *d, SEL c) { return (id)d->x; }
IMPL static id DPad_y(DPad *d, SEL c) { return (id)d->y; }
IMPL static id Pad_a(Pad *p, SEL c) { return (id)p->a; }
IMPL static id Pad_b(Pad *p, SEL c) { return (id)p->b; }
IMPL static id Pad_x(Pad *p, SEL c) { return (id)p->x; }
IMPL static id Pad_y(Pad *p, SEL c) { return (id)p->y; }
IMPL static id Pad_l(Pad *p, SEL c) { return (id)p->l; }
IMPL static id Pad_r(Pad *p, SEL c) { return (id)p->r; }
IMPL static id Pad_dpad(Pad *p, SEL c) { return (id)p->dpad; }
IMPL static id Pad_controller(Pad *p, SEL c) { return (id)g_ctl; }
IMPL static id Ctl_controllers(Class_t *cl, SEL c) { return g_ctl_array; }
IMPL static id Ctl_gamepad(Ctl *k, SEL c) { return (id)k->pad; }
IMPL static id Ctl_nil(Ctl *k, SEL c) { return NULL; }
extern void *ios_Block_copy(const void *b);
IMPL static void Ctl_setPaused(Ctl *k, SEL c, void *blk) { k->paused_block = ios_Block_copy(blk); dslogv("controller: the game registered the pause button"); }
IMPL static int Ctl_attached(Ctl *k, SEL c) { return 0; }
IMPL static int Ctl_index(Ctl *k, SEL c) { return k->index; }
IMPL static void Ctl_setIndex(Ctl *k, SEL c, int i) { k->index = i; }
IMPL static id Ctl_vendor(Ctl *k, SEL c) { return objc_autorelease_(nsstring_from_cstr("PS Vita")); }
IMPL static void Ctl_discover(Class_t *cl, SEL c, void *blk) {}

typedef struct { void *isa; int flags, reserved; void (*invoke)(void *, ...); } BlockHdr;
static int g_shot_requested;
void input_poll_buttons(void) {
  SceCtrlData pad; sceCtrlPeekBufferPositive(0, &pad, 1);
  uint32_t prev = g_buttons;
  g_buttons = pad.buttons; g_lx = pad.lx; g_ly = pad.ly;
  if ((g_buttons & SCE_CTRL_START) && !(prev & SCE_CTRL_START) && !(g_buttons & SCE_CTRL_SELECT) && g_ctl && g_ctl->paused_block) {
    BlockHdr *b = g_ctl->paused_block; b->invoke(b, g_ctl);   // MFi controller pause button
  }
  // START during gameplay: the MFi pause block does not pause in practice; the pause icon is tapped (top
  // left, centre estimated from screenshots). In menus (cursor visible) it does nothing else.
  if ((g_buttons & SCE_CTRL_START) && !(prev & SCE_CTRL_START) && !(g_buttons & SCE_CTRL_SELECT) && !ui_cursor_visible() && g_pause_state == 0) {
    syn_tap(PAUSE_ICON_X, PAUSE_ICON_Y, "Start = Pausa");
    g_pause_state = 1; g_pause_wait = 0;
  }
  // pause tracking: the game shows the menu cursor while paused and hides it when gameplay resumes
  // Pause / Continue state. "Gameplay" signal: the game composes the gameplay screen (gl.c), also
  // during pause and Continue (it is visible behind them). Leaving gameplay clears both states: before, the
  // pause stayed "active" in the menus after quitting, and ○/×/START tapped Resume/Quit.
  { extern int gfx_in_gameplay(void); int cv = ui_cursor_visible(), play = gfx_in_gameplay();
    if (!play) { if (g_pause_state || g_cont_state) dslogv("controller: left gameplay (pause/Continue cleared)"); g_pause_state = 0; g_cont_state = 0; g_play_frames = 0; }
    else {
      if (g_pause_state == 1) { if (cv) g_pause_state = 2; else if (++g_pause_wait > 90) g_pause_state = 0; }
      else if (g_pause_state == 2 && !cv) g_pause_state = 0;
      if (!cv) { g_play_frames++; g_cont_state = 0; }
      else { if (g_play_frames > 60 && g_pause_state == 0) { g_cont_state = 1; dslogv("controller: cursor during gameplay without pause (Continue?) -> ○ = No, START = Yes"); } g_play_frames = 0; }
    } }
  { uint32_t nav = SCE_CTRL_UP | SCE_CTRL_DOWN | SCE_CTRL_LEFT | SCE_CTRL_RIGHT | SCE_CTRL_CROSS | SCE_CTRL_CIRCLE | SCE_CTRL_SQUARE | SCE_CTRL_TRIANGLE;
    if (((g_buttons & ~prev) & nav) || g_lx < 128 - STICK_DEAD || g_lx > 128 + STICK_DEAD || g_ly < 128 - STICK_DEAD || g_ly > 128 + STICK_DEAD)
      ui_cursor_set_hidden(0); }   // using the controller shows the cursor again
  g_swallow &= g_buttons;                                 // releasing the button gives it back to the game
  menu_buttons(g_buttons & ~prev);
  { extern int gfx_in_gameplay(void); g_in_play = gfx_in_gameplay() && !ui_cursor_visible(); }
  // SELECT + L: change control type (without R, which takes a screenshot with SELECT)
  if ((g_buttons & SCE_CTRL_SELECT) && (g_buttons & SCE_CTRL_LTRIGGER) && !(prev & SCE_CTRL_LTRIGGER) && !(g_buttons & SCE_CTRL_RTRIGGER)) {
    g_scheme = (g_scheme + 1) % SCH_N; scheme_save(); g_swallow |= SCE_CTRL_LTRIGGER;
    { char t[40]; snprintf(t, sizeof t, "Controls: Type %s", g_sch_name[g_scheme]);
      extern void gfx_toast(const char *); gfx_toast(t); }
    dslog("controles: cambio a Type %s", g_sch_name[g_scheme]);
  }
  if (!g_scheme && (g_buttons & SCE_CTRL_RTRIGGER) && !(prev & SCE_CTRL_RTRIGGER) && !(g_buttons & (SCE_CTRL_LTRIGGER | SCE_CTRL_SELECT)))
    g_pu_frames = 1;   // Power Up / Lock Shot: tap held on the on-screen button while the physical button stays pressed
  scheme_autoshot();
  // Fix: the controller reader sets "LEFT/RIGHT SHOT held" (player 1 bytes at game 0x51c97a: [0] left,
  // [1] right) and only the on-screen button routine clears that flag (game+0xd370)... if the on-screen button
  // is enabled (u16 at game 0x51c9cc). The game started with Auto Shot ON, so the LEFT/RIGHT SHOT touch buttons
  // are disabled: the flag of the first side used stayed set and the game ignored the other side.
  // It is cleared here just as the game would (before its frame).
  if (g_scheme && g_in_play) {
    uint8_t *G = (uint8_t *)(g_base - g_vmbase);
    uint8_t *held = G + 0x51c97a; uint16_t *en = (uint16_t *)(G + 0x51c9cc);
    if (!en[0]) held[0] = 0;
    if (!en[1]) held[1] = 0;
  }
  { extern int g_draw_dump; uint32_t dd = SCE_CTRL_SELECT | SCE_CTRL_TRIANGLE;
    if (DS_VERBOSE && (g_buttons & dd) == dd && (prev & dd) != dd) g_draw_dump = 2; }   // diagnostics: dump the draw calls of one frame (verbose builds only)
  uint32_t shot = SCE_CTRL_LTRIGGER | SCE_CTRL_RTRIGGER | SCE_CTRL_SELECT;
  if ((g_buttons & shot) == shot && (prev & shot) != shot) g_shot_requested = 1;
}
int input_take_shot_request(void) { int r = g_shot_requested; g_shot_requested = 0; return r; }

static int g_announced;
void notify_post(id note);
void input_announce_controller(void) {   // tells the game that a controller is connected (once)
  if (g_announced) return;
  g_announced = 1;
  extern uintptr_t files_resolve_symbol(const char *);
  id *name = (id *)files_resolve_symbol("_GCControllerDidConnectNotification");
  id note = MSG(MSG((id)objc_getClass("NSNotification"), "notificationWithName:object:", *name, (id)g_ctl), "retain");
  dslogv("controller: posting GCControllerDidConnectNotification");
  notify_post(note);
  objc_release_(note);
}

void input_init(void) {
  scheme_load();
  Class_t *O = g_cls_NSObject;
  C_Set = objc_class_new("NSSet", O, sizeof(Set)); objc_class_new("NSMutableSet", C_Set, sizeof(Set));
  class_add(C_Set, "anyObject", Set_any); class_add(C_Set, "count", Set_count); class_add(C_Set, "allObjects", Set_all);
  class_add(C_Set, "containsObject:", Set_contains); class_add(C_Set, "member:", Set_contains);
  class_add(C_Set, "countByEnumeratingWithState:objects:count:", Set_enum); class_add(C_Set, "dealloc", Set_dealloc);
  class_add_meta(C_Set, "set", Set_newAuto); class_add(C_Set, "addObject:", Set_addObj);
  C_Touch = objc_class_new("UITouch", O, sizeof(Touch));
  class_add(C_Touch, "locationInView:", Touch_loc); class_add(C_Touch, "previousLocationInView:", Touch_prev);
  class_add(C_Touch, "phase", Touch_phase); class_add(C_Touch, "tapCount", Touch_taps);
  class_add(C_Touch, "timestamp", Touch_ts); class_add(C_Touch, "view", Touch_view);
  C_Event = objc_class_new("UIEvent", O, sizeof(Event));
  class_add(C_Event, "allTouches", Event_all); class_add(C_Event, "touchesForView:", Event_forView);

  C_Btn = objc_class_new("GCControllerButtonInput", O, sizeof(Btn));
  objc_class_new("GCControllerAxisInput", C_Btn, sizeof(Btn));
  class_add(C_Btn, "isPressed", Btn_pressed); class_add(C_Btn, "value", Btn_value);
  C_DPad = objc_class_new("GCControllerDirectionPad", O, sizeof(DPad));
  class_add(C_DPad, "up", DPad_up); class_add(C_DPad, "down", DPad_down); class_add(C_DPad, "left", DPad_left);
  class_add(C_DPad, "right", DPad_right); class_add(C_DPad, "xAxis", DPad_x); class_add(C_DPad, "yAxis", DPad_y);
  C_Pad = objc_class_new("GCGamepad", O, sizeof(Pad));
  class_add(C_Pad, "buttonA", Pad_a); class_add(C_Pad, "buttonB", Pad_b); class_add(C_Pad, "buttonX", Pad_x);
  class_add(C_Pad, "buttonY", Pad_y); class_add(C_Pad, "leftShoulder", Pad_l); class_add(C_Pad, "rightShoulder", Pad_r);
  class_add(C_Pad, "dpad", Pad_dpad); class_add(C_Pad, "controller", Pad_controller);
  C_Ctl = objc_getClass("GCController");
  if (!C_Ctl) C_Ctl = objc_class_new("GCController", O, sizeof(Ctl));
  class_add_meta(C_Ctl, "controllers", Ctl_controllers);
  class_add_meta(C_Ctl, "startWirelessControllerDiscoveryWithCompletionHandler:", Ctl_discover);
  class_add_meta(C_Ctl, "stopWirelessControllerDiscovery", Ctl_discover);
  class_add(C_Ctl, "gamepad", Ctl_gamepad); class_add(C_Ctl, "extendedGamepad", Ctl_nil);
  class_add(C_Ctl, "setControllerPausedHandler:", Ctl_setPaused); class_add(C_Ctl, "isAttachedToDevice", Ctl_attached);
  class_add(C_Ctl, "playerIndex", Ctl_index); class_add(C_Ctl, "setPlayerIndex:", Ctl_setIndex);
  class_add(C_Ctl, "vendorName", Ctl_vendor);

  // Mapping: × = A, ○ = B, □ = X, △ = Y, L/R = shoulders, D-pad and left stick = dpad.
  g_ctl = (Ctl *)objc_alloc(C_Ctl, 0);
  Pad *p = g_ctl->pad = (Pad *)objc_alloc(C_Pad, 0);
  p->a = btn(SCE_CTRL_CROSS, 0); p->b = btn(SCE_CTRL_CIRCLE, 0); p->x = btn(SCE_CTRL_SQUARE, 0); p->y = btn(SCE_CTRL_TRIANGLE, 0);
  p->l = btn(SCE_CTRL_LTRIGGER, 0); p->r = btn(SCE_CTRL_RTRIGGER, 0);   // in Type iOS scheme_mask cancels it (R = Power Up)
  DPad *d = p->dpad = (DPad *)objc_alloc(C_DPad, 0);
  d->up = btn(SCE_CTRL_UP, 0); d->down = btn(SCE_CTRL_DOWN, 0); d->left = btn(SCE_CTRL_LEFT, 0); d->right = btn(SCE_CTRL_RIGHT, 0);
  d->x = btn(0, 1); d->y = btn(0, 2);
  g_ctl_array = MSG(MSG((id)objc_getClass("NSArray"), "arrayWithObject:", (id)g_ctl), "retain");
  sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG);
}
