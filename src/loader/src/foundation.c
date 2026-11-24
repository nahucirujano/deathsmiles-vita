// Foundation/UIKit bridge classes: minimal versions of what the game uses from Apple.
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <ctype.h>
#include <psp2/kernel/threadmgr.h>
#include "objc.h"
#include "tstate.h"

// ---------------- NSObject ----------------
IMPL static id NSObject_alloc(Class_t *self, SEL c) { return objc_alloc(self, 0); }
IMPL static id NSObject_allocWithZone(Class_t *self, SEL c, void *z) { return objc_alloc(self, 0); }
IMPL static id NSObject_new(Class_t *self, SEL c) { return MSG(objc_alloc(self, 0), "init"); }
IMPL static id NSObject_self(id self, SEL c) { return self; }
IMPL static id NSObject_retain(id self, SEL c) { return objc_retain_(self); }
IMPL static void NSObject_release(id self, SEL c) { objc_release_(self); }
IMPL static id NSObject_autorelease(id self, SEL c) { return objc_autorelease_(self); }
IMPL static uint32_t NSObject_retainCount(id self, SEL c) { return objc_retain_count(self); }
IMPL static void NSObject_dealloc(id self, SEL c) { objc_free_object(self); }
IMPL static Class_t *NSObject_class(id self, SEL c) { return self->isa; }
IMPL static Class_t *NSObject_superclass(id self, SEL c) { return self->isa->superclass; }
IMPL static Class_t *NSObject_class_cls(Class_t *self, SEL c) { return self; }
IMPL static Class_t *NSObject_superclass_cls(Class_t *self, SEL c) { return self->superclass; }
static int is_kind(Class_t *k, Class_t *c) { for (; k; k = k->superclass) if (k == c) return 1; return 0; }
IMPL static int NSObject_isKindOfClass(id self, SEL c, Class_t *cls) { return is_kind(self->isa, cls); }
IMPL static int NSObject_isMemberOfClass(id self, SEL c, Class_t *cls) { return self->isa == cls; }
IMPL static int NSObject_respondsToSelector(id self, SEL c, SEL s) { return s && class_responds(self->isa, s); }
IMPL static int NSObject_instancesRespond(Class_t *self, SEL c, SEL s) { return s && class_responds(self, s); }
IMPL static int NSObject_isSubclassOfClass(Class_t *self, SEL c, Class_t *o) { return is_kind(self, o); }
IMPL static int NSObject_no(id self, SEL c) { return 0; }
IMPL static uint32_t NSObject_hash(id self, SEL c) { return (uint32_t)(uintptr_t)self >> 2; }
IMPL static int NSObject_isEqual(id self, SEL c, id o) { return self == o; }
IMPL static id NSObject_performSelector(id self, SEL c, SEL s) { return MSG(self, s); }
IMPL static id NSObject_performSelector1(id self, SEL c, SEL s, id a) { return MSG(self, s, a); }
IMPL static id NSObject_performSelector2(id self, SEL c, SEL s, id a, id b) { return MSG(self, s, a, b); }
IMPL static id NSObject_description(id self, SEL c) {
  char b[96]; snprintf(b, sizeof b, "<%s: %p>", class_name(self->isa), (void *)self);
  return objc_autorelease_(nsstring_from_cstr(b));
}
IMPL static id NSObject_description_cls(Class_t *self, SEL c) { return objc_autorelease_(nsstring_from_cstr(class_name(self))); }

// ---------------- NSAutoreleasePool ----------------
typedef struct { Class_t *isa; void *token; } Pool;
IMPL static id Pool_init(Pool *self, SEL c) { self->token = pool_push(); return (id)self; }
IMPL static void Pool_drain(Pool *self, SEL c) { pool_pop(self->token); objc_free_object((id)self); }

// ---------------- NSString (UTF-8 internally; the layout matches CFString constants) ----------------
typedef struct { Class_t *isa; uint32_t flags; const char *str; uint32_t len; } Str;
#define STR_UTF16 0x10
id nsstring_from_cstr(const char *s) {
  Str *o = (Str *)objc_alloc(g_cls_NSString, 0);
  o->flags = 0x7c8; o->len = strlen(s); o->str = memcpy(malloc(o->len + 1), s, o->len + 1);
  return (id)o;
}
static id nsstring_take(char *s) {   // takes ownership of s
  Str *o = (Str *)objc_alloc(g_cls_NSString, 0);
  o->flags = 0x7c8; o->len = strlen(s); o->str = s; return (id)o;
}
const char *nsstring_utf8(id s) { return s ? ((Str *)s)->str : NULL; }
static uint32_t utf16_len(const char *s) {
  uint32_t n = 0;
  for (const uint8_t *p = (const uint8_t *)s; *p; p++) {
    if ((*p & 0xc0) != 0x80) n += ((*p & 0xf8) == 0xf0) ? 2 : 1;
  }
  return n;
}
// converts the executable's UTF-16 constants to UTF-8 (only once, at load time)
void cfstring_fixup(void) {
  const GameSection *s = game_section("__cfstring");
  if (!s) return;
  uint32_t n16 = 0;
  for (Str *c = (Str *)s->addr; (uintptr_t)(c + 1) <= s->addr + s->size; c++) {
    if (!(c->flags & STR_UTF16)) continue;
    const uint16_t *u = (const uint16_t *)c->str;
    char *o = malloc(c->len * 3 + 1), *p = o;
    for (uint32_t i = 0; i < c->len; i++) {
      uint32_t ch = u[i];
      if (ch >= 0xd800 && ch < 0xdc00 && i + 1 < c->len) { ch = 0x10000 + ((ch - 0xd800) << 10) + (u[++i] - 0xdc00); }
      if (ch < 0x80) *p++ = ch;
      else if (ch < 0x800) { *p++ = 0xc0 | ch >> 6; *p++ = 0x80 | (ch & 63); }
      else if (ch < 0x10000) { *p++ = 0xe0 | ch >> 12; *p++ = 0x80 | ((ch >> 6) & 63); *p++ = 0x80 | (ch & 63); }
      else { *p++ = 0xf0 | ch >> 18; *p++ = 0x80 | ((ch >> 12) & 63); *p++ = 0x80 | ((ch >> 6) & 63); *p++ = 0x80 | (ch & 63); }
    }
    *p = 0; c->str = o; c->len = p - o; c->flags = 0x7c8; n16++;
  }
  dslog("cadenas constantes UTF-16 convertidas: %u", n16);
}

// NSString-style formatter: %@ uses -description; the rest is delegated to vsnprintf
char *ns_vformat(const char *fmt, va_list ap) {
  size_t cap = 256, n = 0; char *out = malloc(cap);
  #define PUT(str, l) do { size_t _l = (l); while (n + _l + 1 > cap) out = realloc(out, cap *= 2); memcpy(out + n, str, _l); n += _l; } while (0)
  for (const char *p = fmt; *p; ) {
    if (*p != '%') { const char *q = strchr(p, '%'); size_t l = q ? (size_t)(q - p) : strlen(p); PUT(p, l); p += l; continue; }
    const char *start = p++;
    while (*p && strchr("-+ #0123456789.*", *p)) p++;
    int lng = 0; while (*p == 'l' || *p == 'h' || *p == 'q' || *p == 'z') { if (*p == 'l' || *p == 'q') lng++; p++; }
    char conv = *p ? *p++ : 0, spec[32], tmp[512];
    size_t sl = p - start; if (sl > 31) sl = 31; memcpy(spec, start, sl); spec[sl] = 0;
    if (conv == '@') { id o = va_arg(ap, id); const char *d = o ? nsstring_utf8(MSG(o, "description")) : "(null)"; PUT(d ? d : "(null)", strlen(d ? d : "(null)")); continue; }
    if (conv == '%') { PUT("%", 1); continue; }
    int k;
    // iOS ABI (armv7): 8-byte arguments (long long, double) are 4-byte aligned, without skipping
    // registers. The Vita compiler's va_arg(ap, long long/double) (AAPCS) aligns them to 8 and, depending on the
    // stack layout, read a shifted word: garbage that changed from call to call. This caused the save data reset
    // (the save.dat hash uses "%lld!" and did not match on load). They are read as 2 words.
    if (strchr("diouxXc", conv)) {
      if (lng >= 2) { uint32_t lo = va_arg(ap, uint32_t), hi = va_arg(ap, uint32_t); k = snprintf(tmp, sizeof tmp, spec, (long long)(((uint64_t)hi << 32) | lo)); }
      else k = snprintf(tmp, sizeof tmp, spec, va_arg(ap, int));
    }
    else if (strchr("feEgGaA", conv)) { uint32_t w[2]; w[0] = va_arg(ap, uint32_t); w[1] = va_arg(ap, uint32_t); double dv; memcpy(&dv, w, 8); k = snprintf(tmp, sizeof tmp, spec, dv); }
    else if (conv == 's') k = snprintf(tmp, sizeof tmp, spec, va_arg(ap, char *));
    else if (conv == 'p') k = snprintf(tmp, sizeof tmp, spec, va_arg(ap, void *));
    else { k = 0; }
    if (k > (int)sizeof tmp - 1) k = sizeof tmp - 1;
    if (k > 0) PUT(tmp, k);
  }
  out[n] = 0; return out;
  #undef PUT
}
IMPL static id Str_stringWithFormat(Class_t *self, SEL c, id fmt, ...) {
  va_list ap; va_start(ap, fmt); char *s = ns_vformat(nsstring_utf8(fmt), ap); va_end(ap);
  return objc_autorelease_(nsstring_take(s));
}
IMPL static id Str_initWithFormat(id self, SEL c, id fmt, ...) {
  objc_free_object(self);
  va_list ap; va_start(ap, fmt); char *s = ns_vformat(nsstring_utf8(fmt), ap); va_end(ap);
  return nsstring_take(s);
}
IMPL static id Str_stringWithUTF8String(Class_t *self, SEL c, const char *s) { return s ? objc_autorelease_(nsstring_from_cstr(s)) : NULL; }
IMPL static id Str_stringWithCString(Class_t *self, SEL c, const char *s, uint32_t enc) { return s ? objc_autorelease_(nsstring_from_cstr(s)) : NULL; }
IMPL static id Str_initWithUTF8String(id self, SEL c, const char *s) { objc_free_object(self); return s ? nsstring_from_cstr(s) : NULL; }
IMPL static id Str_initWithCString(id self, SEL c, const char *s, uint32_t e) { objc_free_object(self); return s ? nsstring_from_cstr(s) : NULL; }
IMPL static id Str_string(Class_t *self, SEL c) { return objc_autorelease_(nsstring_from_cstr("")); }
IMPL static id Str_init(id self, SEL c) { objc_free_object(self); return nsstring_from_cstr(""); }
IMPL static uint32_t Str_length(Str *self, SEL c) { return utf16_len(self->str); }
IMPL static const char *Str_UTF8String(Str *self, SEL c) { return self->str; }
IMPL static const char *Str_cString(Str *self, SEL c, uint32_t enc) { return self->str; }
IMPL static int Str_isEqualToString(Str *self, SEL c, Str *o) { return o && !strcmp(self->str, o->str); }
IMPL static int Str_isEqual(Str *self, SEL c, id o) { return o && is_kind(o->isa, g_cls_NSString) && !strcmp(self->str, ((Str *)o)->str); }
IMPL static uint32_t Str_hash(Str *self, SEL c) { uint32_t h = 5381; for (const char *p = self->str; *p; p++) h = h * 33 + (uint8_t)*p; return h; }
IMPL static id Str_description(id self, SEL c) { return self; }
IMPL static id Str_copy(id self, SEL c) { return objc_retain_(self); }
IMPL static id Str_append(Str *self, SEL c, Str *o) {
  const char *b = o ? o->str : ""; char *r = malloc(strlen(self->str) + strlen(b) + 1);
  strcpy(r, self->str); strcat(r, b); return objc_autorelease_(nsstring_take(r));
}
IMPL static int Str_hasPrefix(Str *self, SEL c, Str *o) { return o && !strncmp(self->str, o->str, strlen(o->str)); }
IMPL static int Str_hasSuffix(Str *self, SEL c, Str *o) {
  size_t a = strlen(self->str), b = o ? strlen(o->str) : 0; return o && a >= b && !strcmp(self->str + a - b, o->str);
}
IMPL static int Str_intValue(Str *self, SEL c) { return atoi(self->str); }
IMPL static float Str_floatValue(Str *self, SEL c) { return strtof(self->str, NULL); }
IMPL static void Str_dealloc(Str *self, SEL c) { if (self->flags == 0x7c8) free((void *)self->str); objc_free_object((id)self); }


// --- more NSString (includes NSMutableString and paths) ---
typedef struct { uint32_t loc, len; } NSRange;
static id str_new(Class_t *cls, const char *s) { id o = nsstring_from_cstr(s); o->isa = cls; return o; }
IMPL static id Str_stringWithString(Class_t *self, SEL c, Str *o) { return objc_autorelease_(str_new(self, o ? o->str : "")); }
IMPL static id Str_initWithString(id self, SEL c, Str *o) { Class_t *cls = self->isa; objc_free_object(self); return str_new(cls, o ? o->str : ""); }
IMPL static id Str_mutableCopy(Str *self, SEL c) { return str_new(objc_getClass("NSMutableString"), self->str); }
IMPL static id Str_copyImm(Str *self, SEL c) { return self->isa == objc_getClass("NSMutableString") ? nsstring_from_cstr(self->str) : objc_retain_((id)self); }
static void str_append(Str *s, const char *b) {
  size_t a = strlen(s->str), n = strlen(b);
  char *r = malloc(a + n + 1); memcpy(r, s->str, a); memcpy(r + a, b, n + 1);
  if (s->flags == 0x7c8) free((void *)s->str);
  s->str = r; s->len = a + n; s->flags = 0x7c8;
}
IMPL static void Str_appendString(Str *self, SEL c, Str *o) { if (o) str_append(self, o->str); }
IMPL static void Str_appendFormat(Str *self, SEL c, id fmt, ...) {
  va_list ap; va_start(ap, fmt); char *t = ns_vformat(nsstring_utf8(fmt), ap); va_end(ap);
  str_append(self, t); free(t);
}
IMPL static void Str_setString(Str *self, SEL c, Str *o) { if (self->flags == 0x7c8) free((void *)self->str); self->str = strdup(o ? o->str : ""); self->len = strlen(self->str); self->flags = 0x7c8; }
IMPL static id Str_appendingFormat(Str *self, SEL c, id fmt, ...) {
  va_list ap; va_start(ap, fmt); char *t = ns_vformat(nsstring_utf8(fmt), ap); va_end(ap);
  char *r = malloc(strlen(self->str) + strlen(t) + 1); strcpy(r, self->str); strcat(r, t); free(t);
  return objc_autorelease_(nsstring_take(r));
}
static int cmp_numeric(const char *a, const char *b) {
  while (*a || *b) {
    if (isdigit((unsigned char)*a) && isdigit((unsigned char)*b)) {
      long x = strtol(a, (char **)&a, 10), y = strtol(b, (char **)&b, 10);
      if (x != y) return x < y ? -1 : 1;
    } else { if (*a != *b) return (unsigned char)*a < (unsigned char)*b ? -1 : 1; a++; b++; }
  }
  return 0;
}
IMPL static int Str_compareOpts(Str *self, SEL c, Str *o, uint32_t opts) {
  if (!o) return 1;
  if (opts & 64) return cmp_numeric(self->str, o->str);               // NSNumericSearch
  int r = (opts & 1) ? strcasecmp(self->str, o->str) : strcmp(self->str, o->str);
  return r < 0 ? -1 : r > 0;
}
IMPL static int Str_compare(Str *self, SEL c, Str *o) { return Str_compareOpts(self, c, o, 0); }
IMPL static NSRange Str_rangeOfString(Str *self, SEL c, Str *o) {
  NSRange r = {0x7fffffff, 0};
  const char *p = o ? strstr(self->str, o->str) : NULL;
  if (p) { r.loc = p - self->str; r.len = strlen(o->str); }   // in bytes (valid for ASCII)
  return r;
}
IMPL static id Str_substringFrom(Str *self, SEL c, uint32_t i) { size_t n = strlen(self->str); return objc_autorelease_(nsstring_from_cstr(self->str + (i < n ? i : n))); }
IMPL static id Str_substringTo(Str *self, SEL c, uint32_t i) { char *r = strndup(self->str, i); return objc_autorelease_(nsstring_take(r)); }
IMPL static id Str_substringRange(Str *self, SEL c, NSRange rg) {
  size_t n = strlen(self->str); if (rg.loc > n) rg.loc = n; if (rg.loc + rg.len > n) rg.len = n - rg.loc;
  return objc_autorelease_(nsstring_take(strndup(self->str + rg.loc, rg.len)));
}
IMPL static id Str_appendPath(Str *self, SEL c, Str *o) {
  const char *b = o ? o->str : ""; size_t a = strlen(self->str);
  char *r = malloc(a + strlen(b) + 2); strcpy(r, self->str);
  if (a && r[a - 1] != '/' && *b != '/') strcat(r, "/");
  strcat(r, b); return objc_autorelease_(nsstring_take(r));
}
IMPL static id Str_appendExt(Str *self, SEL c, Str *o) {
  char *r = malloc(strlen(self->str) + (o ? strlen(o->str) : 0) + 2); strcpy(r, self->str); strcat(r, "."); if (o) strcat(r, o->str);
  return objc_autorelease_(nsstring_take(r));
}
IMPL static id Str_lastPath(Str *self, SEL c) { const char *p = strrchr(self->str, '/'); return objc_autorelease_(nsstring_from_cstr(p ? p + 1 : self->str)); }
IMPL static id Str_deletingLastPath(Str *self, SEL c) { const char *p = strrchr(self->str, '/'); return objc_autorelease_(nsstring_take(strndup(self->str, p ? (size_t)(p - self->str) : 0))); }
IMPL static id Str_pathExt(Str *self, SEL c) { const char *p = strrchr(self->str, '.'); return objc_autorelease_(nsstring_from_cstr(p && !strchr(p, '/') ? p + 1 : "")); }
IMPL static id Str_deletingExt(Str *self, SEL c) { const char *p = strrchr(self->str, '.'); return objc_autorelease_(nsstring_take(strndup(self->str, p && !strchr(p, '/') ? (size_t)(p - self->str) : strlen(self->str)))); }
IMPL static id Str_lower(Str *self, SEL c) { char *r = strdup(self->str); for (char *p = r; *p; p++) *p = tolower((unsigned char)*p); return objc_autorelease_(nsstring_take(r)); }
IMPL static id Str_upper(Str *self, SEL c) { char *r = strdup(self->str); for (char *p = r; *p; p++) *p = toupper((unsigned char)*p); return objc_autorelease_(nsstring_take(r)); }
IMPL static uint16_t Str_charAt(Str *self, SEL c, uint32_t i) { return i < strlen(self->str) ? (uint8_t)self->str[i] : 0; }
IMPL static id Str_components(Str *self, SEL c, Str *sep) {
  id arr = MSG((id)objc_getClass("NSMutableArray"), "array");
  const char *p = self->str, *q; size_t sl = sep ? strlen(sep->str) : 0;
  if (!sl) { MSG(arr, "addObject:", (id)self); return arr; }
  while ((q = strstr(p, sep->str))) { MSG(arr, "addObject:", objc_autorelease_(nsstring_take(strndup(p, q - p)))); p = q + sl; }
  MSG(arr, "addObject:", objc_autorelease_(nsstring_from_cstr(p)));
  return arr;
}
IMPL static double Str_doubleValue(Str *self, SEL c) { return strtod(self->str, NULL); }
IMPL static int Str_boolValue(Str *self, SEL c) { char ch = self->str[0]; return ch == 'Y' || ch == 'y' || ch == 'T' || ch == 't' || (ch >= '1' && ch <= '9'); }
IMPL static id Str_withCStringLen(Class_t *self, SEL c, const char *s, uint32_t n) { return objc_autorelease_(nsstring_take(strndup(s, n))); }
IMPL static id Locale_preferred(Class_t *self, SEL c) { return MSG((id)objc_getClass("NSArray"), "arrayWithObject:", objc_autorelease_(nsstring_from_cstr("en"))); }
IMPL static id Str_withChars(Class_t *self, SEL c, const uint16_t *u, uint32_t n) {
  char *o = malloc(n * 3 + 1), *p = o;
  for (uint32_t i = 0; i < n; i++) { uint32_t ch = u[i];
    if (ch < 0x80) *p++ = ch; else if (ch < 0x800) { *p++ = 0xc0 | ch >> 6; *p++ = 0x80 | (ch & 63); }
    else { *p++ = 0xe0 | ch >> 12; *p++ = 0x80 | ((ch >> 6) & 63); *p++ = 0x80 | (ch & 63); } }
  *p = 0; return objc_autorelease_(nsstring_take(o));
}
IMPL static id Str_initBytes(id self, SEL c, const char *b, uint32_t n, uint32_t enc) { objc_free_object(self); return nsstring_take(strndup(b, n)); }
IMPL static uint32_t Str_maxLen(Str *self, SEL c, uint32_t enc) { return strlen(self->str) * 3; }
IMPL static uint32_t Str_replaceOcc(Str *self, SEL c, Str *a, Str *b, uint32_t opts, uint32_t loc, uint32_t len) {
  if (!a || !*a->str) return 0;
  const char *rep = b ? b->str : ""; size_t al = strlen(a->str), bl = strlen(rep); uint32_t n = 0;
  char *out = malloc(strlen(self->str) * (bl > al ? bl / (al ? al : 1) + 1 : 1) + 1), *o = out;
  for (const char *p = self->str; *p;) { if (!strncmp(p, a->str, al)) { memcpy(o, rep, bl); o += bl; p += al; n++; } else *o++ = *p++; }
  *o = 0; if (self->flags == 0x7c8) free((void *)self->str); self->str = out; self->len = strlen(out); self->flags = 0x7c8;
  return n;
}
IMPL static id Exc_raise(id self, SEL c, ...) { dslog("[CPU-bound] [error] the game threw an ObjC exception (%s); ignored", c); return NULL; }
IMPL static id Exc_new(Class_t *self, SEL c, id name, id reason, id info) {
  dslog("[error] NSException %s: %s", name ? nsstring_utf8(name) : "?", reason ? nsstring_utf8(reason) : "?"); return objc_autorelease_(objc_alloc(self, 0));
}
static void string_extras(Class_t *S) {
  class_add_meta(S, "stringWithCharacters:length:", Str_withChars);
  class_add(S, "initWithBytes:length:encoding:", Str_initBytes);
  class_add(S, "maximumLengthOfBytesUsingEncoding:", Str_maxLen);
  class_add(S, "lengthOfBytesUsingEncoding:", Str_maxLen);
  class_add(S, "replaceOccurrencesOfString:withString:options:range:", Str_replaceOcc);
  Class_t *EX = objc_getClass("NSException"); if (!EX) EX = objc_class_new("NSException", g_cls_NSObject, 4);
  class_add_meta(EX, "raise:format:", Exc_raise); class_add_meta(EX, "exceptionWithName:reason:userInfo:", Exc_new);
  class_add(EX, "raise", Exc_raise);
  Class_t *AH = objc_getClass("NSAssertionHandler"); if (!AH) AH = objc_class_new("NSAssertionHandler", g_cls_NSObject, 4);
  class_add_meta(AH, "currentHandler", Exc_new);
  class_add(AH, "handleFailureInFunction:file:lineNumber:description:", Exc_raise);
  class_add(AH, "handleFailureInMethod:object:file:lineNumber:description:", Exc_raise);
  Class_t *L = objc_class_new("NSLocale", g_cls_NSObject, 4); class_add_meta(L, "preferredLanguages", Locale_preferred);
  Class_t *M = objc_getClass("NSMutableString");
  class_add_meta(S, "stringWithString:", Str_stringWithString); class_add(S, "initWithString:", Str_initWithString);
  class_add(S, "mutableCopy", Str_mutableCopy); class_add(S, "copy", Str_copyImm);
  (void)M;   // in this bridge NSString and NSMutableString share the mutable implementation
  class_add(S, "appendString:", Str_appendString); class_add(S, "appendFormat:", Str_appendFormat); class_add(S, "setString:", Str_setString);
  class_add_meta(S, "stringWithCString:length:", Str_withCStringLen);
  class_add(S, "stringByAppendingFormat:", Str_appendingFormat);
  class_add(S, "compare:options:", Str_compareOpts); class_add(S, "compare:", Str_compare);
  class_add(S, "caseInsensitiveCompare:", Str_compare);
  class_add(S, "rangeOfString:", Str_rangeOfString);
  class_add(S, "substringFromIndex:", Str_substringFrom); class_add(S, "substringToIndex:", Str_substringTo);
  class_add(S, "substringWithRange:", Str_substringRange);
  class_add(S, "stringByAppendingPathComponent:", Str_appendPath); class_add(S, "stringByAppendingPathExtension:", Str_appendExt);
  class_add(S, "lastPathComponent", Str_lastPath); class_add(S, "stringByDeletingLastPathComponent", Str_deletingLastPath);
  class_add(S, "pathExtension", Str_pathExt); class_add(S, "stringByDeletingPathExtension", Str_deletingExt);
  class_add(S, "lowercaseString", Str_lower); class_add(S, "uppercaseString", Str_upper);
  class_add(S, "characterAtIndex:", Str_charAt); class_add(S, "componentsSeparatedByString:", Str_components);
  class_add(S, "doubleValue", Str_doubleValue); class_add(S, "boolValue", Str_boolValue);
  class_add(S, "fileSystemRepresentation", Str_UTF8String);
}

// ---------------- NSLog ----------------
static void ios_NSLog(id fmt, ...) {
  va_list ap; va_start(ap, fmt); char *s = ns_vformat(nsstring_utf8(fmt), ap); va_end(ap);
  dslog("NSLog: %s", s); free(s);
}

// ---------------- UIApplication / UIApplicationMain ----------------
static id g_app;
typedef struct { Class_t *isa; id delegate; } App;
IMPL static id App_shared(Class_t *self, SEL c) { return g_app; }
IMPL static id App_delegate(App *self, SEL c) { return self->delegate; }
IMPL static void App_setDelegate(App *self, SEL c, id d) { self->delegate = d; }
void runloop_run(void);
static int ios_UIApplicationMain(int argc, char **argv, id principal, id delegateName) {
  const char *dn = nsstring_utf8(delegateName);
  dslog("UIApplicationMain: delegado=%s", dn ? dn : "(nil)");
  Class_t *appcls = objc_getClass("UIApplication");
  g_app = MSG(MSG((id)appcls, "alloc"), "init");
  Class_t *dc = dn ? objc_getClass(dn) : NULL;
  if (!dc) { dslog("[error] delegate class %s not found", dn ? dn : "(nil)"); return 1; }
  id del = MSG(MSG((id)dc, "alloc"), "init");
  ((App *)g_app)->delegate = del;
  SEL s1 = sel_registerName("application:didFinishLaunchingWithOptions:");
  if (class_responds(del->isa, s1)) { dslog("-> application:didFinishLaunchingWithOptions:"); MSG(del, "application:didFinishLaunchingWithOptions:", g_app, NULL); }
  else if (class_responds(del->isa, sel_registerName("applicationDidFinishLaunching:"))) { dslog("-> applicationDidFinishLaunching:"); MSG(del, "applicationDidFinishLaunching:", g_app); }
  if (class_responds(del->isa, sel_registerName("applicationDidBecomeActive:"))) { dslog("-> applicationDidBecomeActive:"); MSG(del, "applicationDidBecomeActive:", g_app); }
  dslog("delegate ready; entering the main loop");
  runloop_run();
  return 0;
}

uintptr_t bridge_resolve_symbol(const char *name) {
  if (!strcmp(name, "_NSLog")) return (uintptr_t)ios_NSLog;
  if (!strcmp(name, "_CC_MD5")) { extern unsigned char *ios_CC_MD5(const void *, uint32_t, unsigned char *); return (uintptr_t)ios_CC_MD5; }
  if (!strcmp(name, "_UIApplicationMain")) return (uintptr_t)ios_UIApplicationMain;
  uintptr_t r;
  extern uintptr_t misc_resolve_symbol(const char *), cg_resolve_symbol(const char *);
  if ((r = files_resolve_symbol(name)) || (r = audio_resolve_symbol(name)) || (r = misc_resolve_symbol(name)) || (r = cg_resolve_symbol(name))) return r;
  return uikit_resolve_symbol(name);
}

void objc_bridge_init(void) {
  Class_t *O = g_cls_NSObject = objc_class_new("NSObject", NULL, 4);
  class_add_meta(O, "alloc", NSObject_alloc); class_add_meta(O, "allocWithZone:", NSObject_allocWithZone);
  class_add_meta(O, "new", NSObject_new); class_add_meta(O, "class", NSObject_class_cls);
  class_add_meta(O, "superclass", NSObject_superclass_cls); class_add_meta(O, "self", NSObject_self);
  class_add_meta(O, "instancesRespondToSelector:", NSObject_instancesRespond);
  class_add_meta(O, "isSubclassOfClass:", NSObject_isSubclassOfClass);
  class_add_meta(O, "description", NSObject_description_cls);
  class_add_meta(O, "retain", NSObject_self); class_add_meta(O, "release", NSObject_no); class_add_meta(O, "autorelease", NSObject_self);
  class_add(O, "init", NSObject_self); class_add(O, "self", NSObject_self);
  class_add(O, "retain", NSObject_retain); class_add(O, "release", NSObject_release);
  class_add(O, "autorelease", NSObject_autorelease); class_add(O, "retainCount", NSObject_retainCount);
  class_add(O, "dealloc", NSObject_dealloc); class_add(O, "class", NSObject_class); class_add(O, "superclass", NSObject_superclass);
  class_add(O, "isKindOfClass:", NSObject_isKindOfClass); class_add(O, "isMemberOfClass:", NSObject_isMemberOfClass);
  class_add(O, "respondsToSelector:", NSObject_respondsToSelector); class_add(O, "conformsToProtocol:", NSObject_no);
  class_add(O, "hash", NSObject_hash); class_add(O, "isEqual:", NSObject_isEqual); class_add(O, "copy", NSObject_retain);
  class_add(O, "description", NSObject_description);
  class_add(O, "performSelector:", NSObject_performSelector);
  class_add(O, "performSelector:withObject:", NSObject_performSelector1);
  class_add(O, "performSelector:withObject:withObject:", NSObject_performSelector2);

  Class_t *P = objc_class_new("NSAutoreleasePool", O, sizeof(Pool));
  class_add(P, "init", Pool_init); class_add(P, "drain", Pool_drain); class_add(P, "release", Pool_drain);

  Class_t *S = g_cls_NSString = objc_class_new("NSString", O, sizeof(Str));
  g_cls_NSConstantString = objc_class_new("__NSCFConstantString", S, sizeof(Str));
  objc_class_new("NSMutableString", S, sizeof(Str));
  class_add_meta(S, "stringWithFormat:", Str_stringWithFormat); class_add_meta(S, "stringWithUTF8String:", Str_stringWithUTF8String);
  class_add_meta(S, "stringWithCString:encoding:", Str_stringWithCString); class_add_meta(S, "string", Str_string);
  class_add(S, "init", Str_init); class_add(S, "initWithFormat:", Str_initWithFormat);
  class_add(S, "initWithUTF8String:", Str_initWithUTF8String); class_add(S, "initWithCString:encoding:", Str_initWithCString);
  class_add(S, "length", Str_length); class_add(S, "UTF8String", Str_UTF8String); class_add(S, "cStringUsingEncoding:", Str_cString);
  class_add(S, "isEqualToString:", Str_isEqualToString); class_add(S, "isEqual:", Str_isEqual); class_add(S, "hash", Str_hash);
  class_add(S, "description", Str_description); class_add(S, "copy", Str_copy); class_add(S, "stringByAppendingString:", Str_append);
  class_add(S, "hasPrefix:", Str_hasPrefix); class_add(S, "hasSuffix:", Str_hasSuffix);
  class_add(S, "intValue", Str_intValue); class_add(S, "integerValue", Str_intValue); class_add(S, "floatValue", Str_floatValue);
  class_add(S, "dealloc", Str_dealloc);
  string_extras(S);

  Class_t *A = objc_class_new("UIApplication", O, sizeof(App));
  class_add_meta(A, "sharedApplication", App_shared);
  class_add(A, "delegate", App_delegate); class_add(A, "setDelegate:", App_setDelegate);
  collections_init();
  uikit_init();
  threads_init();
  files_init();
  audio_init();
  { extern void misc_init(void), cg_init(void), input_init(void); misc_init(); cg_init(); input_init(); }
  { extern void storekit_init(void); storekit_init(); }
}
