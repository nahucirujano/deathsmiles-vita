// NSKeyedArchiver / NSKeyedUnarchiver with a custom binary format (saves are only written and read on the Vita).
// Entry: [u16 key length][key][type][value]. Types: i=int64, d=double, b=bool, o=object.
// Object: n=nil, S=string, I=integer NSNumber, F=real NSNumber, D=NSData, A=array, M=dictionary,
//         C=custom object (u16 class, u32 length, entries from its encodeWithCoder:).
#include <stdlib.h>
#include <string.h>
#include "objc.h"

typedef struct { uint8_t *p; uint32_t n, cap; } Buf;
static void put(Buf *b, const void *d, uint32_t n) {
  if (b->n + n > b->cap) { b->cap = (b->n + n) * 2 + 64; b->p = realloc(b->p, b->cap); }
  memcpy(b->p + b->n, d, n); b->n += n;
}
static void put32(Buf *b, uint32_t v) { put(b, &v, 4); }
static void putstr16(Buf *b, const char *s) { uint16_t l = strlen(s); put(b, &l, 2); put(b, s, l); }

typedef struct { Class_t *isa; Buf buf; id out; int reading; const uint8_t *rd; uint32_t rdn; } Coder;
static Class_t *C_Arch, *C_Unarch;
static int is_kind(id o, const char *cls) { Class_t *k = objc_getClass(cls); for (Class_t *c = o ? o->isa : NULL; c; c = c->superclass) if (c == k) return 1; return 0; }

static void enc_obj(Buf *b, id o);
static void enc_key(Coder *c, id key, char type) { putstr16(&c->buf, nsstring_utf8(key)); put(&c->buf, &type, 1); }
static void enc_obj(Buf *b, id o) {
  char t;
  if (!o) { t = 'n'; put(b, &t, 1); return; }
  if (is_kind(o, "NSString")) { t = 'S'; put(b, &t, 1); const char *s = nsstring_utf8(o); put32(b, strlen(s)); put(b, s, strlen(s)); return; }
  if (is_kind(o, "NSNumber")) {
    double dv; int64_t iv = (int64_t)MSG(o, "longLongValue");
    typedef IMPL double (*DF)(id, SEL); SEL s = sel_registerName("doubleValue"); dv = ((DF)msg_lookup(o, s))(o, s);
    if (dv != (double)iv) { t = 'F'; put(b, &t, 1); put(b, &dv, 8); } else { t = 'I'; put(b, &t, 1); put(b, &iv, 8); }
    return;
  }
  if (is_kind(o, "NSData")) { t = 'D'; put(b, &t, 1); uint32_t n = (uint32_t)MSG(o, "length"); put32(b, n); put(b, MSG(o, "bytes"), n); return; }
  if (is_kind(o, "NSArray")) {
    t = 'A'; put(b, &t, 1); uint32_t n = (uint32_t)MSG(o, "count"); put32(b, n);
    for (uint32_t i = 0; i < n; i++) enc_obj(b, MSG(o, "objectAtIndex:", i));
    return;
  }
  if (is_kind(o, "NSDictionary")) {
    t = 'M'; put(b, &t, 1); id keys = MSG(o, "allKeys"); uint32_t n = (uint32_t)MSG(keys, "count"); put32(b, n);
    for (uint32_t i = 0; i < n; i++) { id k = MSG(keys, "objectAtIndex:", i); enc_obj(b, k); enc_obj(b, MSG(o, "objectForKey:", k)); }
    return;
  }
  t = 'C'; put(b, &t, 1); putstr16(b, class_name(o->isa));
  Coder *sub = (Coder *)objc_alloc(C_Arch, 0);
  MSG(o, "encodeWithCoder:", (id)sub);
  put32(b, sub->buf.n); put(b, sub->buf.p, sub->buf.n);
  free(sub->buf.p); objc_free_object((id)sub);
}
IMPL static id Arch_initWrite(Coder *c, SEL s, id data) { c->out = objc_retain_(data); return (id)c; }
IMPL static void Arch_encObj(Coder *c, SEL s, id o, id key) { enc_key(c, key, 'o'); enc_obj(&c->buf, o); }
IMPL static void Arch_encInt(Coder *c, SEL s, int v, id key) { int64_t x = v; enc_key(c, key, 'i'); put(&c->buf, &x, 8); }
IMPL static void Arch_encInt64(Coder *c, SEL s, int64_t v, id key) { enc_key(c, key, 'i'); put(&c->buf, &v, 8); }
IMPL static void Arch_encBool(Coder *c, SEL s, int v, id key) { int64_t x = v != 0; enc_key(c, key, 'b'); put(&c->buf, &x, 8); }
IMPL static void Arch_encDouble(Coder *c, SEL s, double v, id key) { enc_key(c, key, 'd'); put(&c->buf, &v, 8); }
IMPL static void Arch_encFloat(Coder *c, SEL s, float v, id key) { double d = v; enc_key(c, key, 'd'); put(&c->buf, &d, 8); }
IMPL static void Arch_finish(Coder *c, SEL s) {
  if (c->out) { static const char magic[4] = {'D', 'S', 'K', '1'}; MSG(c->out, "appendBytes:length:", magic, 4); MSG(c->out, "appendBytes:length:", c->buf.p, c->buf.n); }
}
IMPL static void Coder_dealloc(Coder *c, SEL s) { free(c->buf.p); objc_release_(c->out); objc_free_object((id)c); }
IMPL static id Arch_archivedData(Class_t *cl, SEL s, id root) {
  id data = MSG((id)objc_getClass("NSMutableData"), "data");
  Coder *c = (Coder *)objc_alloc(C_Arch, 0); Arch_initWrite(c, s, data);
  Arch_encObj(c, s, root, objc_autorelease_(nsstring_from_cstr("root"))); Arch_finish(c, s); Coder_dealloc(c, s);
  return data;
}

// ---- lectura ----
static uint16_t rd16(const uint8_t **p) { uint16_t v; memcpy(&v, *p, 2); *p += 2; return v; }
static uint32_t rd32(const uint8_t **p) { uint32_t v; memcpy(&v, *p, 4); *p += 4; return v; }
static id dec_obj(const uint8_t **p, const uint8_t *end);
static const uint8_t *skip_obj(const uint8_t *p, const uint8_t *end) { const uint8_t *q = p; id o = dec_obj(&q, end); objc_release_(o); return q; }
static const uint8_t *find_key(Coder *c, const char *key, char *type) {
  const uint8_t *p = c->rd, *end = c->rd + c->rdn;
  while (p + 3 <= end) {
    uint16_t kl = rd16(&p); const char *k = (const char *)p; p += kl; char t = *p++;
    int match = kl == strlen(key) && !memcmp(k, key, kl);
    if (match) { *type = t; return p; }
    if (t == 'o') p = skip_obj(p, end); else p += 8;
  }
  return NULL;
}
static id dec_obj(const uint8_t **p, const uint8_t *end) {   // devuelve +1 (retenido)
  if (*p >= end) return NULL;
  char t = *(*p)++;
  switch (t) {
    case 'n': return NULL;
    case 'S': { uint32_t n = rd32(p); char *s = strndup((const char *)*p, n); *p += n; id r = nsstring_from_cstr(s); free(s); return r; }
    case 'I': { int64_t v; memcpy(&v, *p, 8); *p += 8; return objc_retain_(MSG((id)objc_getClass("NSNumber"), "numberWithLongLong:", v)); }
    case 'F': { double v; memcpy(&v, *p, 8); *p += 8;
      typedef IMPL id (*NF)(Class_t *, SEL, double); Class_t *N = objc_getClass("NSNumber"); SEL s = sel_registerName("numberWithDouble:");
      return objc_retain_(((NF)class_lookup(N->isa, s))(N, s, v)); }
    case 'D': { uint32_t n = rd32(p); id d = MSG((id)objc_getClass("NSData"), "dataWithBytes:length:", *p, n); *p += n; return objc_retain_(d); }
    case 'A': { uint32_t n = rd32(p); id a = MSG(MSG((id)objc_getClass("NSMutableArray"), "alloc"), "init");
      for (uint32_t i = 0; i < n; i++) { id o = dec_obj(p, end); if (o) { MSG(a, "addObject:", o); objc_release_(o); } }
      return a; }
    case 'M': { uint32_t n = rd32(p); id d = MSG(MSG((id)objc_getClass("NSMutableDictionary"), "alloc"), "init");
      for (uint32_t i = 0; i < n; i++) { id k = dec_obj(p, end), v = dec_obj(p, end); if (k && v) MSG(d, "setObject:forKey:", v, k); objc_release_(k); objc_release_(v); }
      return d; }
    case 'C': {
      uint16_t cl = rd16(p); char name[128]; memcpy(name, *p, cl < 127 ? cl : 127); name[cl < 127 ? cl : 127] = 0; *p += cl;
      uint32_t n = rd32(p);
      Coder *sub = (Coder *)objc_alloc(C_Unarch, 0); sub->reading = 1; sub->rd = *p; sub->rdn = n; *p += n;
      Class_t *k = objc_getClass(name); id o = NULL;
      if (k) o = MSG(MSG((id)k, "alloc"), "initWithCoder:", (id)sub);
      else dslog("[error] unknown class in save data: %s", name);
      objc_free_object((id)sub);
      return o;
    }
  }
  dslog("[error] corrupt save data (type %c)", t);
  *p = end; return NULL;
}
IMPL static id Unarch_initRead(Coder *c, SEL s, id data) {
  c->reading = 1; c->out = objc_retain_(data);
  const uint8_t *b = data ? (const uint8_t *)MSG(data, "bytes") : NULL; uint32_t n = data ? (uint32_t)MSG(data, "length") : 0;
  if (n < 4 || memcmp(b, "DSK1", 4)) { dslog("[warning] save data empty or in another format (%u bytes): ignored", n); c->rd = NULL; c->rdn = 0; }
  else { c->rd = b + 4; c->rdn = n - 4; }
  return (id)c;
}
IMPL static id Unarch_decObj(Coder *c, SEL s, id key) { char t; const uint8_t *p = find_key(c, nsstring_utf8(key), &t); if (!p || t != 'o') return NULL; return objc_autorelease_(dec_obj(&p, c->rd + c->rdn)); }
static int64_t dec_i(Coder *c, id key) { char t; const uint8_t *p = find_key(c, nsstring_utf8(key), &t); int64_t v = 0; if (p && t != 'o') { if (t == 'd') { double d; memcpy(&d, p, 8); v = (int64_t)d; } else memcpy(&v, p, 8); } return v; }
IMPL static int Unarch_decInt(Coder *c, SEL s, id key) { return (int)dec_i(c, key); }
IMPL static int64_t Unarch_decInt64(Coder *c, SEL s, id key) { return dec_i(c, key); }
IMPL static int Unarch_decBool(Coder *c, SEL s, id key) { return dec_i(c, key) != 0; }
IMPL static double Unarch_decDouble(Coder *c, SEL s, id key) {
  char t; const uint8_t *p = find_key(c, nsstring_utf8(key), &t); double d = 0;
  if (p && t == 'd') memcpy(&d, p, 8); else if (p && t != 'o') { int64_t v; memcpy(&v, p, 8); d = v; }
  return d;
}
IMPL static float Unarch_decFloat(Coder *c, SEL s, id key) { return (float)Unarch_decDouble(c, s, key); }
IMPL static int Unarch_contains(Coder *c, SEL s, id key) { char t; return find_key(c, nsstring_utf8(key), &t) != NULL; }
IMPL static void Unarch_finish(Coder *c, SEL s) {}
IMPL static id Unarch_unarchive(Class_t *cl, SEL s, id data) {
  Coder *c = (Coder *)objc_alloc(C_Unarch, 0); Unarch_initRead(c, s, data);
  id r = Unarch_decObj(c, s, objc_autorelease_(nsstring_from_cstr("root"))); Coder_dealloc(c, s); return r;
}

void archive_init(void) {
  Class_t *O = g_cls_NSObject, *Coder_ = objc_class_new("NSCoder", O, sizeof(Coder));
  C_Arch = objc_getClass("NSKeyedArchiver"); if (!C_Arch) C_Arch = objc_class_new("NSKeyedArchiver", Coder_, sizeof(Coder));
  C_Unarch = objc_getClass("NSKeyedUnarchiver"); if (!C_Unarch) C_Unarch = objc_class_new("NSKeyedUnarchiver", Coder_, sizeof(Coder));
  class_add(C_Arch, "initForWritingWithMutableData:", Arch_initWrite);
  class_add(C_Arch, "encodeObject:forKey:", Arch_encObj); class_add(C_Arch, "encodeInt:forKey:", Arch_encInt);
  class_add(C_Arch, "encodeInteger:forKey:", Arch_encInt); class_add(C_Arch, "encodeInt32:forKey:", Arch_encInt);
  class_add(C_Arch, "encodeInt64:forKey:", Arch_encInt64); class_add(C_Arch, "encodeBool:forKey:", Arch_encBool);
  class_add(C_Arch, "encodeDouble:forKey:", Arch_encDouble); class_add(C_Arch, "encodeFloat:forKey:", Arch_encFloat);
  class_add(C_Arch, "finishEncoding", Arch_finish); class_add(C_Arch, "dealloc", Coder_dealloc);
  class_add_meta(C_Arch, "archivedDataWithRootObject:", Arch_archivedData);
  class_add(C_Unarch, "initForReadingWithData:", Unarch_initRead);
  class_add(C_Unarch, "decodeObjectForKey:", Unarch_decObj); class_add(C_Unarch, "decodeIntForKey:", Unarch_decInt);
  class_add(C_Unarch, "decodeIntegerForKey:", Unarch_decInt); class_add(C_Unarch, "decodeInt32ForKey:", Unarch_decInt);
  class_add(C_Unarch, "decodeInt64ForKey:", Unarch_decInt64); class_add(C_Unarch, "decodeBoolForKey:", Unarch_decBool);
  class_add(C_Unarch, "decodeDoubleForKey:", Unarch_decDouble); class_add(C_Unarch, "decodeFloatForKey:", Unarch_decFloat);
  class_add(C_Unarch, "containsValueForKey:", Unarch_contains); class_add(C_Unarch, "finishDecoding", Unarch_finish);
  class_add(C_Unarch, "dealloc", Coder_dealloc);
  class_add_meta(C_Unarch, "unarchiveObjectWithData:", Unarch_unarchive);
}
