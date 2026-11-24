// Minimal Objective-C runtime compatible with the iOS armv7 ABI (objc2, 32-bit).
#pragma once
#include <stdint.h>
#include <stddef.h>
#include "ds.h"

typedef struct objc_class Class_t;
typedef struct objc_object { Class_t *isa; } *id;
typedef const char *SEL;
typedef id (*IMP)(id, SEL, ...);

typedef struct { uint32_t entsize_flags, count; struct { SEL name; const char *types; IMP imp; } m[]; } method_list_t;
typedef struct { uint32_t entsize, count; struct { int32_t *offset; const char *name, *type; uint32_t align, size; } v[]; } ivar_list_t;
typedef struct {
  uint32_t flags, instanceStart, instanceSize;
  const uint8_t *ivarLayout; const char *name;
  method_list_t *baseMethods; void *baseProtocols; ivar_list_t *ivars;
  const uint8_t *weakIvarLayout; void *baseProperties;
} class_ro_t;
struct ClassInfo;
struct objc_class {
  Class_t *isa, *superclass;
  struct ClassInfo *info;   // on iOS: cache (reused here for our method table)
  void *vtable;
  uintptr_t data;           // class_ro_t* (bits bajos = flags)
};
typedef struct { const char *name; Class_t *cls; method_list_t *imeth, *cmeth; void *protos, *props; } category_t;

#define RO(c) ((class_ro_t *)((c)->data & ~3u))
#define RO_META 1

// --- runtime API ---
SEL sel_registerName(const char *name);
Class_t *objc_getClass(const char *name);
Class_t *objc_class_new(const char *name, Class_t *super, uint32_t instance_size);   // bridge class
void class_add(Class_t *cls, const char *sel, void *imp);       // instance method
void class_add_meta(Class_t *cls, const char *sel, void *imp);  // class method
IMP class_lookup(Class_t *cls, SEL sel);
int class_responds(Class_t *cls, SEL sel);
const char *class_name(Class_t *cls);
int class_is_meta(Class_t *cls);
Class_t *obj_class(id o);
uint32_t class_instance_size(Class_t *c);

id   objc_alloc(Class_t *cls, uint32_t extra);
id   objc_retain_(id o);
void objc_release_(id o);
id   objc_autorelease_(id o);
void *pool_push(void);
void pool_pop(void *token);

// Sends a message from C. The variadic pointer forces the base convention (integers in r0-r3),
// same as the game's. Do not pass floats through here.
IMP msg_lookup(id self, SEL sel);
#define MSG(self, sel, ...) ({ id _s = (id)(self); SEL _c = sel_registerName(sel); \
  _s ? ((id (*)(id, SEL, ...))msg_lookup(_s, _c))(_s, _c, ##__VA_ARGS__) : (id)0; })
extern Class_t *g_cls_NSObject, *g_cls_NSString, *g_cls_NSConstantString;

#define IMPL __attribute__((pcs("aapcs")))   // iOS convention for all bridge methods
uintptr_t bridge_resolve_symbol(const char *name);
void bridge_classes_init(void);
id nsstring_from_cstr(const char *s);
const char *nsstring_utf8(id s);
void objc_free_object(id o);
int objc_retain_count(id o);
void collections_init(void);
void uikit_init(void);
uintptr_t uikit_resolve_symbol(const char *name);
void files_init(void);
void audio_init(void);
uintptr_t files_resolve_symbol(const char *name);
uintptr_t audio_resolve_symbol(const char *name);
void runloop_post(id target, SEL sel, id arg);
