// Threads: per-thread state, Darwin pthread, NSThread, sysctlbyname.
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <psp2/kernel/threadmgr.h>
#include "objc.h"
#include "tstate.h"

static TState g_ts[32];
static SceKernelLwMutexWork g_ts_lock_work; static int g_ts_lock_ok;
TState *ts_get(void) {
  SceUID tid = sceKernelGetThreadId();
  for (int i = 0; i < 32; i++) if (g_ts[i].tid == tid) return &g_ts[i];
  if (!g_ts_lock_ok) { sceKernelCreateLwMutex(&g_ts_lock_work, "ts", 0, 0, NULL); g_ts_lock_ok = 1; }
  sceKernelLockLwMutex(&g_ts_lock_work, 1, NULL);
  TState *r = NULL;
  for (int i = 0; i < 32 && !r; i++) if (!g_ts[i].tid) { memset(&g_ts[i], 0, sizeof(TState)); g_ts[i].tid = tid; r = &g_ts[i]; }
  sceKernelUnlockLwMutex(&g_ts_lock_work, 1);
  if (!r) { dslog("[CPU-bound] [error] too many threads"); r = &g_ts[31]; }
  return r;
}
void ts_release(void) { TState *t = ts_get(); free(t->pool); t->pool = NULL; t->tid = 0; }

// ---- Darwin pthread: pthread_t is a pointer (4 bytes), same as in newlib ----
typedef struct { void *(*fn)(void *); void *arg; } Start;
static void *thread_tramp(void *p) {
  STACK_PAD();
  Start s = *(Start *)p; free(p);
  void *r = s.fn(s.arg);
  ts_release();
  return r;
}
int ios_pthread_create(pthread_t *th, const void *darwin_attr, void *(*fn)(void *), void *arg) {
  pthread_attr_t a; pthread_attr_init(&a); pthread_attr_setstacksize(&a, 1024 * 1024);
  Start *s = malloc(sizeof(Start)); s->fn = fn; s->arg = arg;
  int r = pthread_create(th, &a, thread_tramp, s);
  pthread_attr_destroy(&a);
  dslog("pthread_create -> %d (function game+%#x)", r, GAMEADDR(fn));
  return r;
}
// Darwin pthread_mutex_t: {sig, opaque[40]}. A real mutex is stored in opaque[0].
static pthread_mutex_t g_mx_init = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t *dmx(uint32_t *m) {
  pthread_mutex_t *p = (pthread_mutex_t *)(uintptr_t)m[1];
  if (p) return p;
  pthread_mutex_lock(&g_mx_init);
  if (!m[1]) {
    pthread_mutexattr_t a; pthread_mutexattr_init(&a);
    if (m[0] == 0x32AAABA1) pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);   // Darwin recursive initializer
    p = malloc(sizeof(pthread_mutex_t)); pthread_mutex_init(p, &a); m[1] = (uintptr_t)p;
  }
  pthread_mutex_unlock(&g_mx_init);
  return (pthread_mutex_t *)(uintptr_t)m[1];
}
int ios_pthread_mutex_lock(uint32_t *m) { return pthread_mutex_lock(dmx(m)); }
int ios_pthread_mutex_unlock(uint32_t *m) { return pthread_mutex_unlock(dmx(m)); }

// ---- NSThread ----
static SceUID g_tex_tid;   // -[filemanager makeTextureThread] thread
typedef struct { id target; SEL sel; id obj; } NSTStart;
static int nsthread_entry(SceSize n, void *p) {
  STACK_PAD();
  NSTStart s = *(NSTStart *)p;
  void *pool = pool_push();
  dslog("NSThread: -[%s %s]", class_name(s.target->isa), s.sel);
  if (!strcmp((const char *)s.sel, "makeTextureThread")) g_tex_tid = sceKernelGetThreadId();
  MSG(s.target, s.sel, s.obj);
  pool_pop(pool);
  ts_release();
  return sceKernelExitDeleteThread(0);
}
IMPL static void NSThread_detach(Class_t *c, SEL cmd, SEL sel, id target, id obj) {
  NSTStart s = {objc_retain_(target), sel, objc_retain_(obj)};
  // Fixed on core 1 (the main thread is on core 0 and the audio mixer on core 2). The game's only NSThread
  // is the music sequencer, which must not wait behind the main thread.
  SceUID t = sceKernelCreateThread("ds_nsthread", nsthread_entry, 0x10000100, 1024 * 1024, 0, 0x20000 /* core 1 */, NULL);
  sceKernelStartThread(t, sizeof(s), &s);
}
// (Shortening the waits of -[filemanager makeTextureThread] was tried and had no effect.)
IMPL static void NSThread_sleep(Class_t *c, SEL cmd, double s) {
  sceKernelDelayThread((SceUInt)(s * 1e6));
}
// +[NSThread setThreadPriority:] (the music sequencer asks for 1.0): honoured with a high priority
// (96; the main thread is at 64 and the others at 160).
IMPL static int NSThread_setPriority(Class_t *c, SEL cmd, double p) {
  int prio = p >= 0.75 ? 96 : p >= 0.5 ? 128 : 160;
  int r = sceKernelChangeThreadPriority(sceKernelGetThreadId(), prio);
  dslog("NSThread setThreadPriority %.2f -> %d (%08x)", p, prio, r);
  return r >= 0;
}
static SceUID g_main_tid;
void threads_set_main(void) { g_main_tid = sceKernelGetThreadId(); }
int threads_is_main(void) { return sceKernelGetThreadId() == g_main_tid; }
IMPL static int NSThread_isMain(Class_t *c, SEL cmd) { return sceKernelGetThreadId() == g_main_tid; }
static id g_dummy_thread;
IMPL static id NSThread_current(Class_t *c, SEL cmd) { if (!g_dummy_thread) g_dummy_thread = objc_alloc(c, 0); return g_dummy_thread; }

// ---- sysctlbyname("hw.machine") -> iPhone 4 model ----
static int ios_sysctlbyname(const char *name, void *old, size_t *oldlen, void *nw, size_t nl) {
  const char *val = NULL;
  if (!strcmp(name, "hw.machine")) val = "iPhone3,1";
  else if (!strcmp(name, "hw.model")) val = "N90AP";
  if (!val) { dslog("[falta-implementar] sysctlbyname(%s)", name); return -1; }
  size_t n = strlen(val) + 1;
  if (old && oldlen && *oldlen >= n) memcpy(old, val, n);
  if (oldlen) *oldlen = n;
  return 0;
}

uintptr_t threads_resolve_symbol(const char *name) {
  if (!strcmp(name, "_pthread_create")) return (uintptr_t)ios_pthread_create;
  if (!strcmp(name, "_pthread_mutex_lock")) return (uintptr_t)ios_pthread_mutex_lock;
  if (!strcmp(name, "_pthread_mutex_unlock")) return (uintptr_t)ios_pthread_mutex_unlock;
  if (!strcmp(name, "_sysctlbyname")) return (uintptr_t)ios_sysctlbyname;
  return 0;
}
void threads_init(void) {
  Class_t *T = objc_class_new("NSThread", g_cls_NSObject, 4);
  class_add_meta(T, "detachNewThreadSelector:toTarget:withObject:", NSThread_detach);
  class_add_meta(T, "sleepForTimeInterval:", NSThread_sleep);
  class_add_meta(T, "isMainThread", NSThread_isMain); class_add_meta(T, "currentThread", NSThread_current);
  class_add(T, "isMainThread", NSThread_isMain);
  class_add_meta(T, "setThreadPriority:", NSThread_setPriority);
}
