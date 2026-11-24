// Minimal Objective-C runtime to run the iOS executable (32-bit objc2 ABI).
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include "objc.h"
#include "tstate.h"

#define CINFO_MAGIC 0x43494e46u
#define OBJ_MAGIC   0x4f424a31u

typedef struct ClassInfo {
  uint32_t magic;
  Class_t *cls;
  const char *name;
  int meta, initialized, from_game;
  uint32_t inst_size;
  uint32_t n, cap;
  SEL *keys; IMP *vals;
} ClassInfo;

static uint32_t hstr(const char *s) { uint32_t h = 2166136261u; while (*s) h = (h ^ (uint8_t)*s++) * 16777619u; return h; }
static pthread_mutex_t g_rt_lock = PTHREAD_RECURSIVE_MUTEX_INITIALIZER;

// ---------------- selectores ----------------
#define SELCAP 16384
static SEL g_sels[SELCAP];
static SEL sel_find(const char *name, uint32_t *slot) {
  uint32_t i = hstr(name) & (SELCAP - 1);
  while (g_sels[i]) { if (!strcmp(g_sels[i], name)) { *slot = i; return g_sels[i]; } i = (i + 1) & (SELCAP - 1); }
  *slot = i; return NULL;
}
static SEL sel_intern(const char *ptr) {      // registers ptr as canonical if the name is new
  uint32_t slot; SEL s = sel_find(ptr, &slot);
  if (s) return s;
  g_sels[slot] = ptr; return ptr;
}
SEL sel_registerName(const char *name) {
  uint32_t slot; SEL s = sel_find(name, &slot);
  if (s) return s;
  pthread_mutex_lock(&g_rt_lock);
  s = sel_find(name, &slot);
  if (!s) { s = strdup(name); g_sels[slot] = s; }
  pthread_mutex_unlock(&g_rt_lock);
  return s;
}

// ---------------- class registry ----------------
#define CLSCAP 1024
static Class_t *g_classes[CLSCAP];
Class_t *objc_getClass(const char *name) {
  uint32_t i = hstr(name) & (CLSCAP - 1);
  while (g_classes[i]) { if (!strcmp(RO(g_classes[i])->name, name)) return g_classes[i]; i = (i + 1) & (CLSCAP - 1); }
  return NULL;
}
static void class_register(Class_t *c) {
  uint32_t i = hstr(RO(c)->name) & (CLSCAP - 1);
  while (g_classes[i]) { if (g_classes[i] == c) return; i = (i + 1) & (CLSCAP - 1); }
  g_classes[i] = c;
}

static void tbl_put(ClassInfo *ci, SEL k, IMP v) {
  if ((ci->n + 1) * 2 > ci->cap) {
    uint32_t ocap = ci->cap; SEL *ok = ci->keys; IMP *ov = ci->vals;
    ci->cap = ocap ? ocap * 2 : 16; ci->keys = calloc(ci->cap, sizeof(SEL)); ci->vals = calloc(ci->cap, sizeof(IMP)); ci->n = 0;
    for (uint32_t i = 0; i < ocap; i++) if (ok[i]) tbl_put(ci, ok[i], ov[i]);
    // ok/ov are not freed: another thread may be reading them without the lock (tiny leak, only when growing)
  }
  uint32_t i = ((uintptr_t)k >> 2) & (ci->cap - 1);
  while (ci->keys[i] && ci->keys[i] != k) i = (i + 1) & (ci->cap - 1);
  if (!ci->keys[i]) ci->n++;
  ci->vals[i] = v; __atomic_store_n(&ci->keys[i], k, __ATOMIC_RELEASE);
}
static IMP tbl_get(ClassInfo *ci, SEL k) {
  if (!ci->cap) return NULL;
  uint32_t i = ((uintptr_t)k >> 2) & (ci->cap - 1);
  while (ci->keys[i]) { if (ci->keys[i] == k) return ci->vals[i]; i = (i + 1) & (ci->cap - 1); }
  return NULL;
}
static void add_list(ClassInfo *ci, method_list_t *ml) {
  if (!ml) return;
  uint32_t es = ml->entsize_flags & ~3u;
  for (uint32_t i = 0; i < ml->count; i++) {
    uint8_t *e = (uint8_t *)ml->m + i * es;
    tbl_put(ci, *(SEL *)e, *(IMP *)(e + 8));
  }
}
static ClassInfo *info(Class_t *c) {
  ClassInfo *ci = c->info;
  if (ci && ci->magic == CINFO_MAGIC && ci->cls == c) return ci;
  pthread_mutex_lock(&g_rt_lock);
  ci = c->info;
  if (ci && ci->magic == CINFO_MAGIC && ci->cls == c) { pthread_mutex_unlock(&g_rt_lock); return ci; }
  ci = calloc(1, sizeof(ClassInfo));
  ci->magic = CINFO_MAGIC; ci->cls = c; ci->name = RO(c)->name; ci->meta = RO(c)->flags & RO_META;
  ci->inst_size = RO(c)->instanceSize;
  add_list(ci, RO(c)->baseMethods);
  __atomic_store_n(&c->info, ci, __ATOMIC_RELEASE);
  pthread_mutex_unlock(&g_rt_lock);
  return ci;
}

const char *class_name(Class_t *c) { return c ? RO(c)->name : "nil"; }
int class_is_meta(Class_t *c) { return RO(c)->flags & RO_META; }
Class_t *obj_class(id o) { return o ? o->isa : NULL; }
uint32_t class_instance_size(Class_t *c) { return info(c)->inst_size; }
void class_set_instance_size(Class_t *c, uint32_t n) { if (info(c)->inst_size < n) info(c)->inst_size = n; }

Class_t *g_cls_NSObject, *g_cls_NSString, *g_cls_NSConstantString;

Class_t *objc_class_new(const char *name, Class_t *super, uint32_t inst_size) {
  Class_t *c = calloc(1, sizeof(Class_t)), *m = calloc(1, sizeof(Class_t));
  class_ro_t *ro = calloc(1, sizeof(class_ro_t)), *mro = calloc(1, sizeof(class_ro_t));
  ro->name = mro->name = name; mro->flags = RO_META;
  ro->instanceStart = super ? RO(super)->instanceSize : 0;
  ro->instanceSize = inst_size; mro->instanceSize = sizeof(Class_t);
  c->data = (uintptr_t)ro; m->data = (uintptr_t)mro;
  c->isa = m; c->superclass = super;
  if (super) { m->isa = super->isa->isa; m->superclass = super->isa; }
  else { m->isa = m; m->superclass = c; }          // root class
  info(c); info(m);
  class_register(c);
  return c;
}
void class_add(Class_t *c, const char *sel, void *imp) { SEL s = sel_registerName(sel); ClassInfo *ci = info(c); pthread_mutex_lock(&g_rt_lock); tbl_put(ci, s, (IMP)imp); pthread_mutex_unlock(&g_rt_lock); }
void class_add_meta(Class_t *c, const char *sel, void *imp) { class_add(c->isa, sel, imp); }

IMP class_lookup(Class_t *cls, SEL sel) {
  ClassInfo *ci0 = info(cls);
  IMP imp = tbl_get(ci0, sel);
  if (imp) return imp;
  for (Class_t *c = cls->superclass; c; c = c->superclass) {
    imp = tbl_get(info(c), sel);
    if (imp) { pthread_mutex_lock(&g_rt_lock); tbl_put(ci0, sel, imp); pthread_mutex_unlock(&g_rt_lock); return imp; }
  }
  return NULL;
}
int class_responds(Class_t *cls, SEL sel) { return class_lookup(cls, sel) != NULL; }

// ---------------- forwarding of unimplemented messages ----------------
static uint64_t imp_zero(void) { return 0; }
static struct { Class_t *c; SEL s; } g_seen[2048]; static int g_nseen;
static IMP forward(id self, SEL sel) {
  pthread_mutex_lock(&g_rt_lock);
  Class_t *c = self->isa;
  for (int i = 0; i < g_nseen; i++) if (g_seen[i].c == c && g_seen[i].s == sel) { pthread_mutex_unlock(&g_rt_lock); return (IMP)imp_zero; }
  if (g_nseen < 2048) { g_seen[g_nseen].c = c; g_seen[g_nseen].s = sel; g_nseen++; }
  pthread_mutex_unlock(&g_rt_lock);
  int meta = class_is_meta(c);
  dslog("[CPU-bound] [falta-implementar] %c[%s %s]", meta ? '+' : '-', class_name(c), sel);
  return (IMP)imp_zero;
}

static void class_initialize(Class_t *cls) {
  ClassInfo *ci = info(cls);
  if (ci->initialized) return;
  ci->initialized = 1;
  if (cls->superclass) class_initialize(cls->superclass);
  IMP imp = tbl_get(info(cls->isa), sel_registerName("initialize"));
  if (imp) ((id (*)(id, SEL))imp)((id)cls, sel_registerName("initialize"));
}

IMPL static id bad_recv_imp(id self, SEL sel) { return NULL; }
IMP msg_lookup_lr(id self, SEL sel, uint32_t lr) {
  uintptr_t p = (uintptr_t)self;
  if ((p & 3) || p < 0x10000 || p >= 0xF0000000u) {   // invalid receiver (on iOS this would be a crash)
    static int n; if (n++ < 20) dslog("[CPU-bound] [warning] message '%s' to invalid object %p from game+%#x; returning nil",
                                      sel ? sel : "?", self, GAMEADDR(lr));
    return (IMP)bad_recv_imp;
  }
  Class_t *c = self->isa;
  if (class_is_meta(c)) class_initialize((Class_t *)self);
  IMP imp = class_lookup(c, sel);
  return imp ? imp : forward(self, sel);
}
IMP msg_lookup(id self, SEL sel) { return msg_lookup_lr(self, sel, (uint32_t)(uintptr_t)__builtin_return_address(0)); }
IMP msg_lookup_super2(id *sup, SEL sel) {   // sup = {receiver, current class}
  Class_t *start = ((Class_t **)sup)[1]->superclass;
  IMP imp = start ? class_lookup(start, sel) : NULL;
  return imp ? imp : forward(sup[0], sel);
}

// objc_msgSend and variants (ARM). They save r0-r3, align the stack for C code (iOS only guarantees 4 bytes)
// and jump to the IMP through realign_call.
#define ALIGNED_CALL(fn) "  mov r4, sp\n  bic r12, r4, #7\n  mov sp, r12\n  bl " fn "\n  mov sp, r4\n"
__asm__(
  ".arm\n .align 2\n"
  ".global ios_objc_msgSend\n .type ios_objc_msgSend, %function\n ios_objc_msgSend:\n"
  "  cmp r0, #0\n  moveq r1, #0\n  bxeq lr\n"
  "  push {r0-r3, r4, lr}\n  mov r2, lr\n" ALIGNED_CALL("msg_lookup_lr")
  "  mov r12, r0\n  pop {r0-r3, r4, lr}\n  b realign_call\n"
  ".global ios_objc_msgSend_stret\n .type ios_objc_msgSend_stret, %function\n ios_objc_msgSend_stret:\n"
  "  cmp r1, #0\n  bne 1f\n"
  "  push {r0-r3, r4, lr}\n  mov r0, lr\n" ALIGNED_CALL("stret_nil") "  pop {r0-r3, r4, lr}\n  bx lr\n"
  "1:\n  push {r0-r3, r4, lr}\n  mov r0, r1\n  mov r1, r2\n  mov r2, lr\n" ALIGNED_CALL("msg_lookup_lr")
  "  mov r12, r0\n  pop {r0-r3, r4, lr}\n  b realign_call\n"
  ".global ios_objc_msgSendSuper2\n .type ios_objc_msgSendSuper2, %function\n ios_objc_msgSendSuper2:\n"
  "  push {r0-r3, r4, lr}\n" ALIGNED_CALL("msg_lookup_super2")
  "  mov r12, r0\n  pop {r0-r3, r4, lr}\n  ldr r0, [r0]\n  b realign_call\n"
  ".thumb\n"
);
void stret_nil(uint32_t lr) { static int n; if (n++ < 20) dslog("[CPU-bound] [warning] stret message to nil from game+%#x (uninitialized struct)", GAMEADDR(lr)); }
void ios_objc_msgSend(void); void ios_objc_msgSend_stret(void); void ios_objc_msgSendSuper2(void);

// ---------------- object memory ----------------
typedef struct { uint32_t magic; int32_t rc; } ObjHdr;
#define HDR(o) ((ObjHdr *)((uint8_t *)(o) - sizeof(ObjHdr)))
id objc_alloc(Class_t *cls, uint32_t extra) {
  uint32_t sz = class_instance_size(cls) + extra;
  if (sz < 4) sz = 4;
  ObjHdr *h = calloc(1, sizeof(ObjHdr) + sz);
  if (!h) { dslog("[RAM-bound] [error] out of memory creating %s (%u bytes) [memory exhaustion]", class_name(cls), sz); return NULL; }
  h->magic = OBJ_MAGIC; h->rc = 1;
  id o = (id)(h + 1); o->isa = cls; return o;
}
static int is_heap_obj(id o) { return o && ((uintptr_t)o & 3) == 0 && (uintptr_t)o >= 0x81000000u && HDR(o)->magic == OBJ_MAGIC; }
int objc_retain_count(id o) { return is_heap_obj(o) ? HDR(o)->rc : 0x7fffffff; }
id objc_retain_(id o) { if (is_heap_obj(o)) __atomic_add_fetch(&HDR(o)->rc, 1, __ATOMIC_RELAXED); return o; }
void objc_release_(id o) {
  if (!is_heap_obj(o)) return;
  if (__atomic_sub_fetch(&HDR(o)->rc, 1, __ATOMIC_ACQ_REL) == 0) MSG(o, "dealloc");
}
void objc_free_object(id o) { if (is_heap_obj(o)) { HDR(o)->magic = 0; free(HDR(o)); } }

// autorelease pools: one stack per thread
id objc_autorelease_(id o) {
  if (!is_heap_obj(o)) return o;
  TState *t = ts_get();
  if (t->pool_n == t->pool_cap) { t->pool_cap = t->pool_cap ? t->pool_cap * 2 : 1024; t->pool = realloc(t->pool, t->pool_cap * sizeof(void *)); }
  t->pool[t->pool_n++] = o;
  return o;
}
void *pool_push(void) { return (void *)(uintptr_t)ts_get()->pool_n; }
void pool_pop(void *tok) {
  TState *t = ts_get(); uint32_t k = (uintptr_t)tok;
  while (t->pool_n > k) objc_release_((id)t->pool[--t->pool_n]);
}

// ---------------- runtime functions imported by the game ----------------
static id ios_objc_retain(id o) { return objc_retain_(o); }
static id ios_object_getClass(id o) { return (id)obj_class(o); }
static Class_t *ios_class_getSuperclass(Class_t *c) { return c ? c->superclass : NULL; }
static uint32_t ios_class_getInstanceSize(Class_t *c) { return c ? class_instance_size(c) : 0; }
static id ios_objc_getProperty(id self, SEL cmd, int32_t off, int atomic) { return *(id *)((uint8_t *)self + off); }
static void ios_objc_setProperty(id self, SEL cmd, int32_t off, id val, int atomic, int copy) {
  id *slot = (id *)((uint8_t *)self + off);
  id nv = copy ? MSG(val, "copy") : objc_retain_(val);
  id old = *slot; *slot = nv; objc_release_(old);
}
static void ios_objc_copyStruct(void *dst, const void *src, uint32_t size, int atomic, int strong) { memcpy(dst, src, size); }
static int ios_objc_sync(id o) { return 0; }
static Class_t *ios_NSClassFromString(id str) {
  const char *s = (const char *)MSG(str, "UTF8String");
  return s ? objc_getClass(s) : NULL;
}
id nsstring_from_cstr(const char *s);
static id ios_NSStringFromClass(Class_t *c) { return c ? nsstring_from_cstr(class_name(c)) : NULL; }
static id ios_NSStringFromSelector(SEL s) { return s ? nsstring_from_cstr(s) : NULL; }
static void ios_objc_enumerationMutation(id o) { dslog("[error] collection mutated during enumeration"); }

static const struct { const char *n; void *p; } g_rt_syms[] = {
  {"_objc_msgSend", ios_objc_msgSend}, {"_objc_msgSend_stret", ios_objc_msgSend_stret},
  {"_objc_msgSendSuper2", ios_objc_msgSendSuper2}, {"_objc_retain", ios_objc_retain},
  {"_object_getClass", ios_object_getClass}, {"_class_getSuperclass", ios_class_getSuperclass},
  {"_class_getInstanceSize", ios_class_getInstanceSize},
  {"_objc_getProperty", ios_objc_getProperty}, {"_objc_setProperty", ios_objc_setProperty},
  {"_objc_copyStruct", ios_objc_copyStruct}, {"_objc_sync_enter", ios_objc_sync}, {"_objc_sync_exit", ios_objc_sync},
  {"_NSClassFromString", ios_NSClassFromString}, {"_NSStringFromClass", ios_NSStringFromClass},
  {"_NSStringFromSelector", ios_NSStringFromSelector}, {"_objc_enumerationMutation", ios_objc_enumerationMutation},
};

uintptr_t objc_resolve_symbol(const char *name) {
  if (!strncmp(name, "_OBJC_CLASS_$_", 14) || !strncmp(name, "_OBJC_METACLASS_$_", 18)) {
    int meta = name[6] == 'M';
    const char *cn = name + (meta ? 18 : 14);
    Class_t *c = objc_getClass(cn);
    if (!c) {   // Apple class not implemented yet: created empty so the messages it receives are logged
      c = objc_class_new(strdup(cn), g_cls_NSObject, 4);
      dslog("[not-implemented] class %s (created empty)", cn);
    }
    return (uintptr_t)(meta ? c->isa : c);
  }
  if (!strcmp(name, "___CFConstantStringClassReference")) return (uintptr_t)g_cls_NSConstantString;
  for (unsigned i = 0; i < sizeof(g_rt_syms) / sizeof(g_rt_syms[0]); i++)
    if (!strcmp(g_rt_syms[i].n, name)) return (uintptr_t)g_rt_syms[i].p;
  uintptr_t r = threads_resolve_symbol(name);
  return r ? r : bridge_resolve_symbol(name);
}

// ---------------- loading the game's metadata ----------------
static void canon_sel(SEL *ref) { *ref = sel_intern(*ref); }
static void canon_list(method_list_t *ml) {
  if (!ml) return;
  uint32_t es = ml->entsize_flags & ~3u;
  for (uint32_t i = 0; i < ml->count; i++) canon_sel((SEL *)((uint8_t *)ml->m + i * es));
}
void objc_load_image(void) {
  const GameSection *s;
  uint32_t nsel = 0, ncls = 0, ncat = 0;
  if ((s = game_section("__objc_selrefs")))
    for (uint32_t i = 0; i < s->size / 4; i++, nsel++) canon_sel((SEL *)(s->addr + 4 * i));
  if ((s = game_section("__objc_classlist")))
    for (uint32_t i = 0; i < s->size / 4; i++, ncls++) {
      Class_t *c = ((Class_t **)s->addr)[i];
      canon_list(RO(c)->baseMethods); canon_list(RO(c->isa)->baseMethods);
      c->info = NULL; c->isa->info = NULL;     // apuntaban a _objc_empty_cache
      info(c)->from_game = 1; info(c->isa)->from_game = 1;
      class_register(c);
    }
  if ((s = game_section("__objc_catlist")))
    for (uint32_t i = 0; i < s->size / 4; i++, ncat++) {
      category_t *cat = ((category_t **)s->addr)[i];
      canon_list(cat->imeth); canon_list(cat->cmeth);
      add_list(info(cat->cls), cat->imeth); add_list(info(cat->cls->isa), cat->cmeth);
      dslog("category %s on %s", cat->name, class_name(cat->cls));
    }
  void cfstring_fixup(void); cfstring_fixup();
  dslog("ObjC runtime: %u selectors, %u game classes, %u categories", nsel, ncls, ncat);
}
