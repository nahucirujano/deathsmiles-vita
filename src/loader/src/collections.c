// NSNumber, NSNull, NSArray/NSMutableArray, NSDictionary/NSMutableDictionary (minimal versions).
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include "objc.h"

static int obj_equal(id a, id b) { return a == b || (a && b && MSG(a, "isEqual:", b)); }

// ---------------- NSNumber ----------------
typedef struct { Class_t *isa; int isfloat; int64_t i; double d; } Num;
static Class_t *g_num;
static id num_i(int64_t v) { Num *n = (Num *)objc_alloc(g_num, 0); n->i = v; n->d = (double)v; return objc_autorelease_((id)n); }
static id num_d(double v) { Num *n = (Num *)objc_alloc(g_num, 0); n->isfloat = 1; n->d = v; n->i = (int64_t)v; return objc_autorelease_((id)n); }
IMPL static id Num_withInt(Class_t *s, SEL c, int v) { return num_i(v); }
IMPL static id Num_withUInt(Class_t *s, SEL c, unsigned v) { return num_i(v); }
IMPL static id Num_withLL(Class_t *s, SEL c, long long v) { return num_i(v); }
IMPL static id Num_withBool(Class_t *s, SEL c, signed char v) { return num_i(v != 0); }
IMPL static id Num_withFloat(Class_t *s, SEL c, float v) { return num_d(v); }
IMPL static id Num_withDouble(Class_t *s, SEL c, double v) { return num_d(v); }
IMPL static int Num_int(Num *n, SEL c) { return (int)n->i; }
IMPL static long long Num_ll(Num *n, SEL c) { return n->i; }
IMPL static int Num_bool(Num *n, SEL c) { return n->isfloat ? n->d != 0 : n->i != 0; }
IMPL static float Num_float(Num *n, SEL c) { return (float)n->d; }
IMPL static double Num_double(Num *n, SEL c) { return n->d; }
IMPL static int Num_isEqual(Num *n, SEL c, Num *o) { return o && o->isa == n->isa && (n->isfloat || o->isfloat ? n->d == o->d : n->i == o->i); }
IMPL static uint32_t Num_hash(Num *n, SEL c) { return (uint32_t)n->i; }
IMPL static int Num_compare(Num *n, SEL c, Num *o) { double a = n->d, b = o ? o->d : 0; return a < b ? -1 : a > b; }
IMPL static id Num_description(Num *n, SEL c) {
  char b[48]; if (n->isfloat) snprintf(b, sizeof b, "%g", n->d); else snprintf(b, sizeof b, "%lld", (long long)n->i);
  return objc_autorelease_(nsstring_from_cstr(b));
}

// ---------------- NSNull ----------------
static id g_null;
IMPL static id Null_null(Class_t *s, SEL c) { return g_null; }

// ---------------- NSArray ----------------
typedef struct { Class_t *isa; uint32_t n, cap; id *v; uint32_t mut; } Arr;
static Class_t *g_arr, *g_marr;
static void arr_push(Arr *a, id o) {
  if (a->n == a->cap) { a->cap = a->cap ? a->cap * 2 : 8; a->v = realloc(a->v, a->cap * sizeof(id)); }
  a->v[a->n++] = objc_retain_(o);
}
static Arr *arr_new(Class_t *cls) { return (Arr *)objc_alloc(cls, 0); }
IMPL static id Arr_array(Class_t *s, SEL c) { return objc_autorelease_((id)arr_new(s)); }
IMPL static id Arr_withObject(Class_t *s, SEL c, id o) { Arr *a = arr_new(s); arr_push(a, o); return objc_autorelease_((id)a); }
IMPL static id Arr_withObjects(Class_t *s, SEL c, id first, ...) {
  Arr *a = arr_new(s); va_list ap; va_start(ap, first);
  for (id o = first; o; o = va_arg(ap, id)) arr_push(a, o);
  va_end(ap); return objc_autorelease_((id)a);
}
IMPL static id Arr_withArray(Class_t *s, SEL c, Arr *src) { Arr *a = arr_new(s); for (uint32_t i = 0; src && i < src->n; i++) arr_push(a, src->v[i]); return objc_autorelease_((id)a); }
IMPL static id Arr_init(Arr *a, SEL c) { return (id)a; }
IMPL static id Arr_initCap(Arr *a, SEL c, uint32_t n) { return (id)a; }
IMPL static id Arr_initWithArray(Arr *a, SEL c, Arr *src) { for (uint32_t i = 0; src && i < src->n; i++) arr_push(a, src->v[i]); return (id)a; }
IMPL static uint32_t Arr_count(Arr *a, SEL c) { return a->n; }
IMPL static id Arr_at(Arr *a, SEL c, uint32_t i) {
  if (i >= a->n) { dslog("[CPU-bound] [error] -[NSArray objectAtIndex:%u] out of range (%u)", i, a->n); return NULL; }
  return a->v[i];
}
IMPL static id Arr_last(Arr *a, SEL c) { return a->n ? a->v[a->n - 1] : NULL; }
IMPL static id Arr_first(Arr *a, SEL c) { return a->n ? a->v[0] : NULL; }
IMPL static uint32_t Arr_indexOf(Arr *a, SEL c, id o) { for (uint32_t i = 0; i < a->n; i++) if (obj_equal(a->v[i], o)) return i; return 0x7fffffff; }
IMPL static int Arr_contains(Arr *a, SEL c, id o) { return Arr_indexOf(a, c, o) != 0x7fffffff; }
IMPL static void Arr_add(Arr *a, SEL c, id o) { if (o) arr_push(a, o); }
IMPL static void Arr_insert(Arr *a, SEL c, id o, uint32_t i) {
  if (!o) return; arr_push(a, o); if (i >= a->n) return;
  memmove(a->v + i + 1, a->v + i, (a->n - 1 - i) * sizeof(id)); a->v[i] = o;
}
IMPL static void Arr_removeAt(Arr *a, SEL c, uint32_t i) {
  if (i >= a->n) return; id o = a->v[i];
  memmove(a->v + i, a->v + i + 1, (a->n - i - 1) * sizeof(id)); a->n--; objc_release_(o);
}
IMPL static void Arr_remove(Arr *a, SEL c, id o) { for (uint32_t i = a->n; i-- > 0;) if (obj_equal(a->v[i], o)) Arr_removeAt(a, c, i); }
IMPL static void Arr_removeLast(Arr *a, SEL c) { if (a->n) Arr_removeAt(a, c, a->n - 1); }
IMPL static void Arr_removeAll(Arr *a, SEL c) { while (a->n) Arr_removeAt(a, c, a->n - 1); }
IMPL static void Arr_replace(Arr *a, SEL c, uint32_t i, id o) { if (i < a->n && o) { id old = a->v[i]; a->v[i] = objc_retain_(o); objc_release_(old); } }
IMPL static void Arr_addFrom(Arr *a, SEL c, Arr *src) { for (uint32_t i = 0; src && i < src->n; i++) arr_push(a, src->v[i]); }
IMPL static void Arr_perform(Arr *a, SEL c, SEL s) { for (uint32_t i = 0; i < a->n; i++) MSG(a->v[i], s); }
IMPL static void Arr_perform1(Arr *a, SEL c, SEL s, id x) { for (uint32_t i = 0; i < a->n; i++) MSG(a->v[i], s, x); }
IMPL static id Arr_copy(Arr *a, SEL c) { Arr *b = arr_new(g_arr); for (uint32_t i = 0; i < a->n; i++) arr_push(b, a->v[i]); return (id)b; }
IMPL static id Arr_mcopy(Arr *a, SEL c) { Arr *b = arr_new(g_marr); for (uint32_t i = 0; i < a->n; i++) arr_push(b, a->v[i]); return (id)b; }
typedef struct { uint32_t state; id *items; uint32_t *mut; uint32_t extra[5]; } FastEnum;
static uint32_t g_mut_dummy;
IMPL static uint32_t Arr_enumerate(Arr *a, SEL c, FastEnum *st, id *buf, uint32_t len) {
  if (st->state) return 0;
  st->state = 1; st->items = a->v; st->mut = &g_mut_dummy; return a->n;
}
IMPL static void Arr_dealloc(Arr *a, SEL c) { for (uint32_t i = 0; i < a->n; i++) objc_release_(a->v[i]); free(a->v); objc_free_object((id)a); }
IMPL static id Arr_description(Arr *a, SEL c) {
  char b[64]; snprintf(b, sizeof b, "(NSArray with %u elements)", a->n); return objc_autorelease_(nsstring_from_cstr(b));
}

// ---------------- NSDictionary ----------------
typedef struct { Class_t *isa; uint32_t n, cap; id *k, *v; } Dict;
static Class_t *g_dict, *g_mdict;
static int dict_find(Dict *d, id k) { for (uint32_t i = 0; i < d->n; i++) if (obj_equal(d->k[i], k)) return i; return -1; }
static void dict_set(Dict *d, id v, id k) {
  if (!k || !v) return;
  int i = dict_find(d, k);
  if (i >= 0) { id old = d->v[i]; d->v[i] = objc_retain_(v); objc_release_(old); return; }
  if (d->n == d->cap) { d->cap = d->cap ? d->cap * 2 : 8; d->k = realloc(d->k, d->cap * sizeof(id)); d->v = realloc(d->v, d->cap * sizeof(id)); }
  d->k[d->n] = MSG(k, "copy"); d->v[d->n] = objc_retain_(v); d->n++;
}
IMPL static id Dict_dict(Class_t *s, SEL c) { return objc_autorelease_(objc_alloc(s, 0)); }
IMPL static id Dict_withObjectForKey(Class_t *s, SEL c, id v, id k) { Dict *d = (Dict *)objc_alloc(s, 0); dict_set(d, v, k); return objc_autorelease_((id)d); }
IMPL static id Dict_withObjectsAndKeys(Class_t *s, SEL c, id first, ...) {
  Dict *d = (Dict *)objc_alloc(s, 0); va_list ap; va_start(ap, first);
  for (id v = first; v; v = va_arg(ap, id)) { id k = va_arg(ap, id); dict_set(d, v, k); }
  va_end(ap); return objc_autorelease_((id)d);
}
IMPL static id Dict_withDict(Class_t *s, SEL c, Dict *src) { Dict *d = (Dict *)objc_alloc(s, 0); for (uint32_t i = 0; src && i < src->n; i++) dict_set(d, src->v[i], src->k[i]); return objc_autorelease_((id)d); }
IMPL static id Dict_init(Dict *d, SEL c) { return (id)d; }
IMPL static id Dict_initCap(Dict *d, SEL c, uint32_t n) { return (id)d; }
IMPL static uint32_t Dict_count(Dict *d, SEL c) { return d->n; }
IMPL static id Dict_get(Dict *d, SEL c, id k) { int i = dict_find(d, k); return i >= 0 ? d->v[i] : NULL; }
IMPL static void Dict_set(Dict *d, SEL c, id v, id k) { dict_set(d, v, k); }
IMPL static void Dict_remove(Dict *d, SEL c, id k) {
  int i = dict_find(d, k); if (i < 0) return;
  id ok = d->k[i], ov = d->v[i]; d->n--; d->k[i] = d->k[d->n]; d->v[i] = d->v[d->n];
  objc_release_(ok); objc_release_(ov);
}
IMPL static void Dict_removeAll(Dict *d, SEL c) { while (d->n) Dict_remove(d, c, d->k[d->n - 1]); }
IMPL static id Dict_allKeys(Dict *d, SEL c) { Arr *a = arr_new(g_arr); for (uint32_t i = 0; i < d->n; i++) arr_push(a, d->k[i]); return objc_autorelease_((id)a); }
IMPL static id Dict_allValues(Dict *d, SEL c) { Arr *a = arr_new(g_arr); for (uint32_t i = 0; i < d->n; i++) arr_push(a, d->v[i]); return objc_autorelease_((id)a); }
IMPL static uint32_t Dict_enumerate(Dict *d, SEL c, FastEnum *st, id *buf, uint32_t len) {
  if (st->state) return 0; st->state = 1; st->items = d->k; st->mut = &g_mut_dummy; return d->n;
}
IMPL static id Dict_copy(Dict *d, SEL c) { return objc_retain_(Dict_withDict(g_dict, c, d)); }
IMPL static id Dict_mcopy(Dict *d, SEL c) { return objc_retain_(Dict_withDict(g_mdict, c, d)); }
IMPL static void Dict_dealloc(Dict *d, SEL c) {
  for (uint32_t i = 0; i < d->n; i++) { objc_release_(d->k[i]); objc_release_(d->v[i]); }
  free(d->k); free(d->v); objc_free_object((id)d);
}

// NSArray enumerators and extras
typedef struct { Class_t *isa; Arr *a; uint32_t i; } Enum;
static Class_t *g_enum;
IMPL static id Arr_objectEnumerator(Arr *a, SEL c) { Enum *e = (Enum *)objc_alloc(g_enum, 0); e->a = (Arr *)objc_retain_((id)a); return objc_autorelease_((id)e); }
IMPL static id Enum_next(Enum *e, SEL c) { return e->i < e->a->n ? e->a->v[e->i++] : NULL; }
IMPL static void Enum_dealloc(Enum *e, SEL c) { objc_release_((id)e->a); objc_free_object((id)e); }
IMPL static id Arr_initWithObjects(Arr *a, SEL c, id first, ...) {
  va_list ap; va_start(ap, first); for (id o = first; o; o = va_arg(ap, id)) arr_push(a, o); va_end(ap); return (id)a;
}
static int (*g_sort_fn)(id, id, void *); static void *g_sort_ctx;
static int sort_cmp(const void *x, const void *y) { return g_sort_fn(*(id *)x, *(id *)y, g_sort_ctx); }
IMPL static void Arr_sortFn(Arr *a, SEL c, int (*fn)(id, id, void *), void *ctx) { g_sort_fn = fn; g_sort_ctx = ctx; qsort(a->v, a->n, sizeof(id), sort_cmp); }
IMPL static id Dict_keyEnumerator(Dict *d, SEL c) { return Arr_objectEnumerator((Arr *)Dict_allKeys(d, c), c); }
IMPL static id Dict_objectEnumerator(Dict *d, SEL c) { return Arr_objectEnumerator((Arr *)Dict_allValues(d, c), c); }

void collections_init(void) {
  Class_t *O = g_cls_NSObject;
  Class_t *N = g_num = objc_class_new("NSNumber", O, sizeof(Num));
  objc_class_new("NSValue", O, 4);
  class_add_meta(N, "numberWithInt:", Num_withInt); class_add_meta(N, "numberWithInteger:", Num_withInt);
  class_add_meta(N, "numberWithUnsignedInt:", Num_withUInt); class_add_meta(N, "numberWithUnsignedInteger:", Num_withUInt);
  class_add_meta(N, "numberWithLong:", Num_withInt); class_add_meta(N, "numberWithLongLong:", Num_withLL);
  class_add_meta(N, "numberWithUnsignedLongLong:", Num_withLL);
  class_add_meta(N, "numberWithBool:", Num_withBool); class_add_meta(N, "numberWithChar:", Num_withBool);
  class_add_meta(N, "numberWithFloat:", Num_withFloat); class_add_meta(N, "numberWithDouble:", Num_withDouble);
  class_add(N, "intValue", Num_int); class_add(N, "integerValue", Num_int); class_add(N, "unsignedIntValue", Num_int);
  class_add(N, "unsignedIntegerValue", Num_int); class_add(N, "longValue", Num_int); class_add(N, "charValue", Num_int);
  class_add(N, "longLongValue", Num_ll); class_add(N, "unsignedLongLongValue", Num_ll);
  class_add(N, "boolValue", Num_bool); class_add(N, "floatValue", Num_float); class_add(N, "doubleValue", Num_double);
  class_add(N, "isEqual:", Num_isEqual); class_add(N, "isEqualToNumber:", Num_isEqual); class_add(N, "hash", Num_hash);
  class_add(N, "compare:", Num_compare); class_add(N, "description", Num_description); class_add(N, "stringValue", Num_description);
  class_add(N, "copy", (void *)class_lookup(O, sel_registerName("retain")));

  Class_t *NU = objc_class_new("NSNull", O, 4);
  g_null = objc_alloc(NU, 0); class_add_meta(NU, "null", Null_null);

  Class_t *A = g_arr = objc_class_new("NSArray", O, sizeof(Arr));
  Class_t *MA = g_marr = objc_class_new("NSMutableArray", A, sizeof(Arr));
  (void)MA;
  class_add_meta(A, "array", Arr_array); class_add_meta(A, "arrayWithObject:", Arr_withObject);
  class_add_meta(A, "arrayWithObjects:", Arr_withObjects); class_add_meta(A, "arrayWithArray:", Arr_withArray);
  class_add_meta(A, "arrayWithCapacity:", Arr_array);
  class_add(A, "init", Arr_init); class_add(A, "initWithCapacity:", Arr_initCap); class_add(A, "initWithArray:", Arr_initWithArray);
  class_add(A, "count", Arr_count); class_add(A, "objectAtIndex:", Arr_at); class_add(A, "objectAtIndexedSubscript:", Arr_at);
  class_add(A, "lastObject", Arr_last); class_add(A, "firstObject", Arr_first);
  class_add(A, "indexOfObject:", Arr_indexOf); class_add(A, "containsObject:", Arr_contains);
  class_add(A, "makeObjectsPerformSelector:", Arr_perform); class_add(A, "makeObjectsPerformSelector:withObject:", Arr_perform1);
  class_add(A, "copy", Arr_copy); class_add(A, "mutableCopy", Arr_mcopy);
  class_add(A, "countByEnumeratingWithState:objects:count:", Arr_enumerate);
  class_add(A, "dealloc", Arr_dealloc); class_add(A, "description", Arr_description);
  class_add(A, "addObject:", Arr_add); class_add(A, "insertObject:atIndex:", Arr_insert);
  class_add(A, "removeObjectAtIndex:", Arr_removeAt); class_add(A, "removeObject:", Arr_remove);
  class_add(A, "removeLastObject", Arr_removeLast); class_add(A, "removeAllObjects", Arr_removeAll);
  class_add(A, "replaceObjectAtIndex:withObject:", Arr_replace); class_add(A, "setObject:atIndexedSubscript:", Arr_replace);
  class_add(A, "addObjectsFromArray:", Arr_addFrom);
  g_enum = objc_class_new("NSEnumerator", O, sizeof(Enum));
  class_add(g_enum, "nextObject", Enum_next); class_add(g_enum, "dealloc", Enum_dealloc);
  class_add(A, "objectEnumerator", Arr_objectEnumerator); class_add(A, "initWithObjects:", Arr_initWithObjects);
  class_add(A, "setObject:atIndex:", Arr_replace); class_add(A, "sortUsingFunction:context:", Arr_sortFn);

  Class_t *D = g_dict = objc_class_new("NSDictionary", O, sizeof(Dict));
  g_mdict = objc_class_new("NSMutableDictionary", D, sizeof(Dict));
  class_add_meta(D, "dictionary", Dict_dict); class_add_meta(D, "dictionaryWithCapacity:", Dict_dict);
  class_add_meta(D, "dictionaryWithObject:forKey:", Dict_withObjectForKey);
  class_add_meta(D, "dictionaryWithObjectsAndKeys:", Dict_withObjectsAndKeys);
  class_add_meta(D, "dictionaryWithDictionary:", Dict_withDict);
  class_add(D, "init", Dict_init); class_add(D, "initWithCapacity:", Dict_initCap);
  class_add(D, "count", Dict_count); class_add(D, "objectForKey:", Dict_get); class_add(D, "valueForKey:", Dict_get);
  class_add(D, "objectForKeyedSubscript:", Dict_get);
  class_add(D, "setObject:forKey:", Dict_set); class_add(D, "setValue:forKey:", Dict_set); class_add(D, "setObject:forKeyedSubscript:", Dict_set);
  class_add(D, "removeObjectForKey:", Dict_remove); class_add(D, "removeAllObjects", Dict_removeAll);
  class_add(D, "allKeys", Dict_allKeys); class_add(D, "allValues", Dict_allValues);
  class_add(D, "countByEnumeratingWithState:objects:count:", Dict_enumerate);
  class_add(D, "keyEnumerator", Dict_keyEnumerator); class_add(D, "objectEnumerator", Dict_objectEnumerator);
  class_add(D, "copy", Dict_copy); class_add(D, "mutableCopy", Dict_mcopy); class_add(D, "dealloc", Dict_dealloc);
}
