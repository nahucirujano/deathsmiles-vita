// Files and data: NSBundle, NSData, NSFileHandle, NSFileManager, NSURL, NSNotificationCenter, NSDate.
// The contents of deathsmiles.app go in ux0:data/deathsmiles/app/ and save data in ux0:data/deathsmiles/Documents/.
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/io/dirent.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/processmgr.h>
#include "objc.h"
#include "tstate.h"

#define APP_DIR "ux0:data/deathsmiles/app"
#define DOC_DIR "ux0:data/deathsmiles/Documents"
static void sav_hook(void);
static id S(const char *s) { return objc_autorelease_(nsstring_from_cstr(s)); }
static Class_t *C_Data, *C_MData, *C_Handle, *C_URL, *C_Note;

static int path_exists(const char *p, int *isdir) {
  SceIoStat st; if (sceIoGetstat(p, &st) < 0) return 0;
  if (isdir) *isdir = SCE_S_ISDIR(st.st_mode); return 1;
}

// ---------------- NSBundle ----------------
static id g_bundle;
IMPL static id Bundle_main(Class_t *c, SEL s) { if (!g_bundle) g_bundle = objc_alloc(c, 0); return g_bundle; }
IMPL static id Bundle_path(id b, SEL s) { return S(APP_DIR); }
IMPL static id Bundle_pathFor(id b, SEL s, id name, id type) {
  const char *n = nsstring_utf8(name), *t = nsstring_utf8(type);
  char p[512]; snprintf(p, sizeof p, "%s/%s%s%s", APP_DIR, n ? n : "", t && *t ? "." : "", t ? t : "");
  if (!path_exists(p, NULL)) { dslog("[warning] pathForResource: %s not found", p); return NULL; }
  return S(p);
}
IMPL static id Bundle_info(id b, SEL s) {
  return MSG((id)objc_getClass("NSDictionary"), "dictionaryWithObjectsAndKeys:", S("1.0.19"), S("CFBundleShortVersionString"),
             S("1.0.19"), S("CFBundleVersion"), S("co.jp.cave.deathsmiles"), S("CFBundleIdentifier"), NULL);
}
static void *read_whole(const char *path, uint32_t *n);
// Localizable.strings (binary plist) -> dictionary. en.lproj (English) is used.
static id g_strings;
static uint64_t be(const uint8_t *p, int n) { uint64_t v = 0; while (n--) v = v << 8 | *p++; return v; }
static id bp_obj(const uint8_t *d, uint32_t sz, const uint8_t *offs, int osz, int rsz, uint64_t idx, int depth) {
  if (depth > 3) return NULL;
  const uint8_t *o = d + be(offs + idx * osz, osz); if (o >= d + sz) return NULL;
  int t = *o >> 4, n = *o & 15; const uint8_t *p = o + 1;
  if ((t == 5 || t == 6 || t == 0xd) && n == 15) { int is = 1 << (*p & 15); n = (int)be(p + 1, is); p += 1 + is; }
  if (t == 5) { char *c = strndup((const char *)p, n); id r = nsstring_from_cstr(c); free(c); return r; }
  if (t == 6) {
    char *u = malloc(n * 3 + 1), *q = u;
    for (int i = 0; i < n; i++) { uint32_t ch = p[i * 2] << 8 | p[i * 2 + 1];
      if (ch < 0x80) *q++ = ch; else if (ch < 0x800) { *q++ = 0xc0 | ch >> 6; *q++ = 0x80 | (ch & 63); }
      else { *q++ = 0xe0 | ch >> 12; *q++ = 0x80 | ((ch >> 6) & 63); *q++ = 0x80 | (ch & 63); } }
    *q = 0; id r = nsstring_from_cstr(u); free(u); return r;
  }
  if (t == 0xd) {
    id dict = MSG(MSG((id)objc_getClass("NSMutableDictionary"), "alloc"), "init");
    for (int i = 0; i < n; i++) {
      id k = bp_obj(d, sz, offs, osz, rsz, be(p + i * rsz, rsz), depth + 1), v = bp_obj(d, sz, offs, osz, rsz, be(p + (n + i) * rsz, rsz), depth + 1);
      if (k && v) MSG(dict, "setObject:forKey:", v, k);
      objc_release_(k); objc_release_(v);
    }
    return dict;
  }
  return NULL;
}
static void load_strings(void) {
  uint32_t n; uint8_t *d = read_whole(APP_DIR "/en.lproj/Localizable.strings", &n);
  if (!d || n < 40 || memcmp(d, "bplist00", 8)) { dslog("[warning] could not read Localizable.strings"); g_strings = MSG(MSG((id)objc_getClass("NSMutableDictionary"), "alloc"), "init"); free(d); return; }
  const uint8_t *tr = d + n - 32; int osz = tr[6], rsz = tr[7];
  uint64_t top = be(tr + 16, 8), offt = be(tr + 24, 8);
  g_strings = bp_obj(d, n, d + offt, osz, rsz, top, 0);
  free(d);
  dslogv("game texts (English): %u entries", g_strings ? (unsigned)(uintptr_t)MSG(g_strings, "count") : 0);
}
IMPL static id Bundle_localized(id b, SEL c, id key, id value, id table) {
  if (!g_strings) load_strings();
  id r = key ? MSG(g_strings, "objectForKey:", key) : NULL;
  if (r) return r;
  return value && MSG(value, "length") ? value : key;
}
IMPL static id Bundle_ident(id b, SEL c) { return S("co.jp.cave.deathsmiles"); }
IMPL static id Bundle_infoKey(id b, SEL s, id k) { return MSG(Bundle_info(b, s), "objectForKey:", k); }

// ---------------- NSData / NSMutableData ----------------
typedef struct { Class_t *isa; uint8_t *bytes; uint32_t len, cap; } Data;
static Data *data_new(Class_t *c, const void *b, uint32_t n) {
  Data *d = (Data *)objc_alloc(c, 0); d->cap = n ? n : 16; d->bytes = malloc(d->cap); d->len = n;
  if (b && n) memcpy(d->bytes, b, n); else if (n) memset(d->bytes, 0, n);
  return d;
}
static void note_file(const char *path) {   // remembers the last data*.bin read (texture diagnostics)
  const char *b = strrchr(path, '/'); b = b ? b + 1 : path;
  if (!strncmp(b, "data", 4)) { TState *t = ts_get(); strncpy(t->last_file, b, sizeof t->last_file - 1); }
}
// ---- Read-ahead of dataNNNN.bin ----
// Measured: each stage texture = read the whole file (~200 ms per 2 MB, the card gives ~10 MB/s)
// + decode it in the game (~290 ms), one after the other. The files of one load are usually consecutive (data0632, 0633…).
// After reading dataNNNN.bin, dataNNNN+1.bin is read on another thread while the game decodes; if the game
// asks for it, it is served from memory. If it asks for another file, the read-ahead is discarded. At most 8 MiB in memory.
static SceKernelLwMutexWork g_pf_mx; static int g_pf_init;
static char g_pf_path[256]; static uint8_t *volatile g_pf_buf; static volatile uint32_t g_pf_len; static volatile int g_pf_state;   // 0 free, 1 reading, 2 ready
static uint32_t g_pf_hits, g_pf_miss; static uint64_t g_pf_saved_us, g_pf_wait_us;
static void pf_lock(void) { if (!g_pf_init) { sceKernelCreateLwMutex(&g_pf_mx, "pf", 0, 0, NULL); g_pf_init = 1; } sceKernelLockLwMutex(&g_pf_mx, 1, NULL); }
static void pf_unlock(void) { sceKernelUnlockLwMutex(&g_pf_mx, 1); }
static int pf_thread(SceSize n, void *p) {
  char path[256]; pf_lock(); strcpy(path, g_pf_path); pf_unlock();
  uint64_t t0 = sceKernelGetProcessTimeWide();
  SceUID fd = sceIoOpen(path, SCE_O_RDONLY, 0); uint8_t *b = NULL; int sz = -1;
  if (fd >= 0) {
    sz = sceIoLseek(fd, 0, SCE_SEEK_END); sceIoLseek(fd, 0, SCE_SEEK_SET);
    if (sz > 0 && sz <= 8 * 1024 * 1024 && (b = malloc(sz)) && sceIoRead(fd, b, sz) != sz) { free(b); b = NULL; }
    sceIoClose(fd);
  }
  pf_lock();
  if (g_pf_state == 1 && !strcmp(path, g_pf_path) && b) { g_pf_buf = b; g_pf_len = sz; g_pf_state = 2; g_pf_saved_us += sceKernelGetProcessTimeWide() - t0; }
  else { free(b); if (g_pf_state == 1 && !strcmp(path, g_pf_path)) g_pf_state = 0; }
  pf_unlock();
  return sceKernelExitDeleteThread(0);
}
static void pf_start_next(const char *path) {
  const char *b = strrchr(path, '/'); b = b ? b + 1 : path;
  int num; if (strncmp(b, "data", 4) || sscanf(b + 4, "%4d.bin", &num) != 1 || strlen(b) != 12) return;
  char next[256]; snprintf(next, sizeof next, "%.*sdata%04d.bin", (int)(b - path), path, num + 1);
  SceIoStat st; if (sceIoGetstat(next, &st) < 0 || st.st_size > 8 * 1024 * 1024) return;
  pf_lock();
  if (g_pf_state == 1) { pf_unlock(); return; }               // a read is already in progress
  if (g_pf_state == 2) { free(g_pf_buf); g_pf_buf = NULL; g_pf_miss++; }   // unused read-ahead
  strcpy(g_pf_path, next); g_pf_state = 1;
  pf_unlock();
  SceUID t = sceKernelCreateThread("ds_prefetch", pf_thread, 0x10000100, 0x4000, 0, 0x40000 /* core 2 */, NULL);
  if (t < 0 || sceKernelStartThread(t, 0, NULL) < 0) { pf_lock(); g_pf_state = 0; pf_unlock(); }
}
// If the requested file is the one read ahead, hands over the buffer (waits if it is still being read).
static uint8_t *pf_take(const char *path, uint32_t *len) {
  pf_lock();
  if (!g_pf_state || strcmp(path, g_pf_path)) { pf_unlock(); return NULL; }
  uint64_t w0 = sceKernelGetProcessTimeWide();
  while (g_pf_state == 1) { pf_unlock(); sceKernelDelayThread(500); pf_lock(); if (strcmp(path, g_pf_path)) { pf_unlock(); return NULL; } }
  g_pf_wait_us += sceKernelGetProcessTimeWide() - w0;
  uint8_t *b = NULL;
  if (g_pf_state == 2) { b = g_pf_buf; *len = g_pf_len; g_pf_buf = NULL; g_pf_state = 0; g_pf_hits++; }
  pf_unlock();
  return b;
}
static void *read_whole(const char *path, uint32_t *n) {
  note_file(path);
  { uint32_t pl; uint8_t *pb = pf_take(path, &pl); if (pb) { *n = pl; pf_start_next(path); return pb; } }
  SceUID fd = sceIoOpen(path, SCE_O_RDONLY, 0);
  if (fd < 0) return NULL;
  uint32_t sz = sceIoLseek(fd, 0, SCE_SEEK_END); sceIoLseek(fd, 0, SCE_SEEK_SET);
  void *b = malloc(sz ? sz : 1);
  if (!b) { dslog("[RAM-bound] [error] out of memory reading %s (%u bytes) [memory exhaustion]", path, sz); sceIoClose(fd); return NULL; }
  uint64_t t0 = sceKernelGetProcessTimeWide();
  sceIoRead(fd, b, sz); sceIoClose(fd); *n = sz;
  if (sz >= 256 * 1024) pf_start_next(path);
  if (sz >= 256 * 1024) { const char *nm = strrchr(path, '/'); dslogv("[OK] file read (whole) %s: %u KiB in %.1f ms (thread %x)", nm ? nm + 1 : path, sz / 1024, (sceKernelGetProcessTimeWide() - t0) / 1000.0, sceKernelGetThreadId()); }
  return b;
}
IMPL static id Data_withFile(Class_t *c, SEL s, id path) { sav_hook();
  uint32_t n; void *b = read_whole(nsstring_utf8(path), &n);
  if (!b) { dslog("[warning] dataWithContentsOfFile: %s not found", nsstring_utf8(path)); return NULL; }
  Data *d = (Data *)objc_alloc(c, 0); d->bytes = b; d->len = d->cap = n; return objc_autorelease_((id)d);
}
IMPL static id Data_initWithFile(id self, SEL s, id path) { Class_t *c = self->isa; objc_free_object(self); id r = Data_withFile(c, s, path); return objc_retain_(r); }
IMPL static id Data_withBytes(Class_t *c, SEL s, const void *b, uint32_t n) { return objc_autorelease_((id)data_new(c, b, n)); }
IMPL static id Data_withLength(Class_t *c, SEL s, uint32_t n) { return objc_autorelease_((id)data_new(c, NULL, n)); }
IMPL static id Data_data(Class_t *c, SEL s) { return objc_autorelease_((id)data_new(c, NULL, 0)); }
IMPL static id Data_init(Data *d, SEL s) { if (!d->bytes) { d->cap = 16; d->bytes = malloc(16); } return (id)d; }
IMPL static id Data_initWithData(Data *d, SEL s, Data *o) { free(d->bytes); d->len = o ? o->len : 0; d->cap = d->len ? d->len : 16; d->bytes = malloc(d->cap); if (d->len) memcpy(d->bytes, o->bytes, d->len); return (id)d; }
IMPL static id Data_withData(Class_t *c, SEL s, Data *o) { return objc_autorelease_((id)data_new(c, o ? o->bytes : NULL, o ? o->len : 0)); }
IMPL static void *Data_bytes(Data *d, SEL s) { return d->bytes; }
IMPL static uint32_t Data_length(Data *d, SEL s) { return d->len; }
IMPL static void Data_getBytes(Data *d, SEL s, void *out, uint32_t n) { memcpy(out, d->bytes, n < d->len ? n : d->len); }
IMPL static void Data_getAll(Data *d, SEL s, void *out) { memcpy(out, d->bytes, d->len); }
IMPL static void Data_getRange(Data *d, SEL s, void *out, uint32_t loc, uint32_t n) { if (loc < d->len) memcpy(out, d->bytes + loc, n < d->len - loc ? n : d->len - loc); }
IMPL static id Data_initBytes(Data *d, SEL s, const void *b, uint32_t n) {
  free(d->bytes); d->cap = n ? n : 16; d->bytes = malloc(d->cap); d->len = n; if (n) memcpy(d->bytes, b, n); return (id)d;
}
IMPL static id Data_initBytesNoCopy(Data *d, SEL s, void *b, uint32_t n) { free(d->bytes); d->bytes = b; d->len = d->cap = n; return (id)d; }
IMPL static id Data_initBytesNoCopyFree(Data *d, SEL s, void *b, uint32_t n, int fr) { return Data_initBytesNoCopy(d, s, fr ? b : memcpy(malloc(n ? n : 1), b, n), n); }
IMPL static id Data_withBytesNoCopy(Class_t *c, SEL s, void *b, uint32_t n) { Data *d = (Data *)objc_alloc(c, 0); d->bytes = b; d->len = d->cap = n; return objc_autorelease_((id)d); }
IMPL static id Data_initLength(Data *d, SEL s, uint32_t n) { free(d->bytes); d->bytes = calloc(1, n ? n : 1); d->len = n; d->cap = n ? n : 1; return (id)d; }
IMPL static id Data_sub(Data *d, SEL s, uint32_t loc, uint32_t n) { if (loc > d->len) loc = d->len; if (n > d->len - loc) n = d->len - loc; return objc_autorelease_((id)data_new(C_Data, d->bytes + loc, n)); }
IMPL static int Data_isEqual(Data *d, SEL s, Data *o) { return o && o->len == d->len && !memcmp(o->bytes, d->bytes, d->len); }
IMPL static void Data_append(Data *d, SEL s, const void *b, uint32_t n) {
  if (d->len + n > d->cap) { d->cap = (d->len + n) * 2; d->bytes = realloc(d->bytes, d->cap); }
  memcpy(d->bytes + d->len, b, n); d->len += n;
}
IMPL static void Data_appendData(Data *d, SEL s, Data *o) { if (o) Data_append(d, s, o->bytes, o->len); }
IMPL static void Data_setLength(Data *d, SEL s, uint32_t n) {
  if (n > d->cap) { d->bytes = realloc(d->bytes, n); memset(d->bytes + d->cap, 0, n - d->cap); d->cap = n; }
  d->len = n;
}
// Atomic write with a backup. Before, the file was truncated and overwritten: if the Vita closed the game
// (PS button -> close, battery) in the middle of a save, save.dat was cut short and the game showed
// "セーブデータ初期化" (save data reset). Now: <file>.tmp is written, the previous one becomes <file>.bak
// and the .tmp is renamed to the final name. At startup, files_repair() restores the .bak if the main file is missing or damaged.
IMPL static int Data_write(Data *d, SEL s, id path, int atomic) {
  const char *p = nsstring_utf8(path); char tmp[512], bak[512];
  snprintf(tmp, sizeof tmp, "%s.tmp", p); snprintf(bak, sizeof bak, "%s.bak", p);
  SceUID fd = sceIoOpen(tmp, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
  if (fd < 0) { dslog("[error] could not write %s", tmp); return 0; }
  int w = sceIoWrite(fd, d->bytes, d->len); sceIoClose(fd);
  if (w != (int)d->len) { dslog("[error] incomplete write of %s (%d of %u bytes) [RAM-bound: memory card full?]", p, w, d->len); sceIoRemove(tmp); return 0; }
  if (path_exists(p, NULL)) { sceIoRemove(bak); sceIoRename(p, bak); }
  if (sceIoRename(tmp, p) < 0) { dslog("[error] could not rename %s -> %s", tmp, p); return 0; }
  return 1;
}
IMPL static void Data_dealloc(Data *d, SEL s) { free(d->bytes); objc_free_object((id)d); }
IMPL static id Data_copy(Data *d, SEL s) { return (id)data_new(C_Data, d->bytes, d->len); }
IMPL static id Data_mcopy(Data *d, SEL s) { return (id)data_new(C_MData, d->bytes, d->len); }
IMPL static id Str_dataUsingEncoding(id str, SEL s, uint32_t enc) { const char *u = nsstring_utf8(str); return objc_autorelease_((id)data_new(C_Data, u, strlen(u))); }
IMPL static id Str_initWithData(id self, SEL s, Data *d, uint32_t enc) {
  objc_free_object(self); char *t = malloc(d->len + 1); memcpy(t, d->bytes, d->len); t[d->len] = 0;
  id r = nsstring_from_cstr(t); free(t); return r;
}

// ---------------- NSFileHandle ----------------
typedef struct { Class_t *isa; SceUID fd; uint8_t *cbuf; uint32_t clen, cpos; char *path; } Handle;
IMPL static id Handle_forReading(Class_t *c, SEL s, id path) {
  note_file(nsstring_utf8(path));
  SceUID fd = sceIoOpen(nsstring_utf8(path), SCE_O_RDONLY, 0);
  if (fd < 0) { dslog("[warning] fileHandleForReadingAtPath: %s not found", nsstring_utf8(path)); return NULL; }
  Handle *h = (Handle *)objc_alloc(c, 0); h->fd = fd; h->path = strdup(nsstring_utf8(path));
  h->cbuf = pf_take(h->path, &h->clen); h->cpos = 0;
  return objc_autorelease_((id)h);
}
static uint32_t g_hr_calls; static uint64_t g_hr_bytes, g_hr_us; static volatile uint64_t g_note_post_t;
// NSFileHandle read summary (called every 5 s from uikit.c; verbose builds only)
void files_read_report(void) {
  if (g_hr_calls) dslogv("%s file handle reads: %u calls, %llu KiB, %.0f ms in 5 s%s", g_hr_calls > 200 ? "[bandwidth-bound]" : "[OK]", g_hr_calls,
                        g_hr_bytes / 1024, g_hr_us / 1000.0, g_hr_calls > 200 ? " [bandwidth-bound: many small reads from the memory card]" : "");
  if (g_pf_hits || g_pf_miss)
    dslogv("[OK] prefetch: %u hits (card reads hidden: %.0f ms, game waited %.0f ms), %u unused", g_pf_hits, g_pf_saved_us / 1000.0, g_pf_wait_us / 1000.0, g_pf_miss);
  g_pf_hits = g_pf_miss = 0; g_pf_saved_us = g_pf_wait_us = 0;
  g_hr_calls = 0; g_hr_bytes = g_hr_us = 0;
}
void files_note_delivered(void) {
  uint64_t t = g_note_post_t; if (!t) return; g_note_post_t = 0;
  double ms = (sceKernelGetProcessTimeWide() - t) / 1000.0;
  dslogv("%s background read: notification delivered %.1f ms after the read finished%s", ms > 50 ? "[CPU-bound]" : "[OK]", ms,
        ms > 50 ? " [CPU-bound: main-thread stall, run loop not draining]" : "");
}
static id handle_read(Handle *h, uint32_t n) {
  if (h->cbuf) {   // read-ahead file: served from memory
    uint32_t left = h->clen - h->cpos; if (n > left) n = left;
    Data *d;
    if (h->cpos == 0 && n == h->clen) { d = (Data *)objc_alloc(C_Data, 0); d->bytes = h->cbuf; d->len = d->cap = n; h->cbuf = NULL; }
    else { d = data_new(C_Data, NULL, n); memcpy(d->bytes, h->cbuf + h->cpos, n); d->len = n; }
    h->cpos += n;
    if (n >= 256 * 1024 && h->path) pf_start_next(h->path);
    return (id)d;
  }
  if (n == 0xffffffff) { uint32_t cur = sceIoLseek(h->fd, 0, SCE_SEEK_CUR); n = sceIoLseek(h->fd, 0, SCE_SEEK_END) - cur; sceIoLseek(h->fd, cur, SCE_SEEK_SET); }
  Data *d = data_new(C_Data, NULL, n);
  uint64_t t0 = sceKernelGetProcessTimeWide();
  int r = sceIoRead(h->fd, d->bytes, n); d->len = r > 0 ? r : 0;
  uint64_t dt = sceKernelGetProcessTimeWide() - t0;
  g_hr_calls++; g_hr_bytes += d->len; g_hr_us += dt;
  if (d->len >= 256 * 1024 && h->path) pf_start_next(h->path);
  if (d->len >= 256 * 1024) dslogv("[OK] file read (handle) %u KiB in %.1f ms (thread %x)", d->len / 1024, dt / 1000.0, sceKernelGetThreadId());
  return (id)d;
}
IMPL static id Handle_readToEnd(Handle *h, SEL s) { return objc_autorelease_(handle_read(h, 0xffffffff)); }
IMPL static id Handle_readLen(Handle *h, SEL s, uint32_t n) { return objc_autorelease_(handle_read(h, n)); }
IMPL static void Handle_seek(Handle *h, SEL s, uint64_t off) { sceIoLseek(h->fd, off, SCE_SEEK_SET); h->cpos = off < h->clen ? off : h->clen; }
IMPL static void Handle_close(Handle *h, SEL s) { if (h->fd >= 0) sceIoClose(h->fd); h->fd = -1; free(h->cbuf); h->cbuf = NULL; }
IMPL static void Handle_dealloc(Handle *h, SEL s) { if (h->fd >= 0) sceIoClose(h->fd); free(h->cbuf); free(h->path); objc_free_object((id)h); }
static id g_note_read_done, g_note_data_item;
static id note_new(id name, id obj, id info);
void notify_post(id note);
static int bg_read_thread(SceSize n, void *p) {
  STACK_PAD();
  Handle *h = *(Handle **)p;
  id d = handle_read(h, 0xffffffff);
  id info = MSG((id)objc_getClass("NSDictionary"), "dictionaryWithObject:forKey:", d, g_note_data_item);
  objc_retain_(info); objc_release_(d);
  id note = note_new(g_note_read_done, (id)h, info);
  g_note_post_t = sceKernelGetProcessTimeWide();
  runloop_post(NULL, (SEL)"__post_note", note);
  return sceKernelExitDeleteThread(0);
}
IMPL static void Handle_readBg(Handle *h, SEL s) {
  objc_retain_((id)h);
  SceUID t = sceKernelCreateThread("ds_fileread", bg_read_thread, 0x10000100, 256 * 1024, 0, 0, NULL);
  sceKernelStartThread(t, sizeof(h), &h);
}

// ---------------- NSFileManager / rutas ----------------
static id g_fm;
IMPL static id FM_default(Class_t *c, SEL s) { if (!g_fm) g_fm = objc_alloc(c, 0); return g_fm; }
IMPL static int FM_exists(id fm, SEL s, id path) { sav_hook(); return path && path_exists(nsstring_utf8(path), NULL); }
IMPL static int FM_existsDir(id fm, SEL s, id path, signed char *isdir) { int d = 0, r = path && path_exists(nsstring_utf8(path), &d); if (isdir) *isdir = d; return r; }
IMPL static id FM_attrs(id fm, SEL s, id path, id *err) {
  if (err) *err = NULL;
  SceIoStat st; if (!path || sceIoGetstat(nsstring_utf8(path), &st) < 0) return NULL;
  id num = MSG((id)objc_getClass("NSNumber"), "numberWithLongLong:", (long long)st.st_size);
  return MSG((id)objc_getClass("NSDictionary"), "dictionaryWithObject:forKey:", num, S("NSFileSize"));
}
IMPL static uint64_t Dict_fileSize(id d, SEL s) { id n = MSG(d, "objectForKey:", S("NSFileSize")); return n ? (uint64_t)(long long)MSG(n, "longLongValue") : 0; }
IMPL static int FM_remove(id fm, SEL s, id path, id *err) { if (err) *err = NULL; return path && sceIoRemove(nsstring_utf8(path)) >= 0; }
IMPL static int FM_mkdir(id fm, SEL s, id path, int inter, id attrs, id *err) { if (err) *err = NULL; sceIoMkdir(nsstring_utf8(path), 0777); return 1; }
// -[savedata checkSavedata]: the game compares an MD5 of all fields with the one stored in save.dat; if it
// does not match, it deletes the save. Early development builds formatted "%lld" wrongly (iOS ABI, see
// foundation.c) and saves from those sessions have a hash computed from garbage. If the file exists and is
// structurally sound, it is accepted anyway (and logged) instead of losing the save.
static int file_ok(const char *p);
typedef IMPL int (*CheckFn)(id, SEL);
static CheckFn g_check_orig;
IMPL static int Sav_check(id self, SEL c) {
  int r = g_check_orig(self, c);
  if (!r && file_ok(DOC_DIR "/save.dat")) { dslog("[warning] save: the save.dat hash does not match (saved with an old faulty format); the save is accepted"); return 1; }
  return r;
}
static void sav_hook(void) {
  static int done; if (done) return;
  Class_t *k = objc_getClass("savedata"); if (!k) return;
  IMP o = class_lookup(k, sel_registerName("checkSavedata")); done = 1;
  if (!o) return;
  g_check_orig = (CheckFn)o; class_add(k, "checkSavedata", Sav_check);
}
static id ios_NSSearchPathForDirectoriesInDomains(int dir, int mask, int expand) {
  sav_hook();
  sceIoMkdir(DOC_DIR, 0777);
  const char *p = dir == 13 ? "ux0:data/deathsmiles/Caches" : DOC_DIR;   // 13 = NSCachesDirectory
  if (dir == 13) sceIoMkdir(p, 0777);
  return MSG((id)objc_getClass("NSArray"), "arrayWithObject:", S(p));
}

// ---------------- NSURL ----------------
typedef struct { Class_t *isa; id path; } URL;
IMPL static id URL_fileURL(Class_t *c, SEL s, id path) { URL *u = (URL *)objc_alloc(c, 0); u->path = objc_retain_(MSG(path, "copy")); return objc_autorelease_((id)u); }
IMPL static id URL_path(URL *u, SEL s) { return u->path; }
const char *url_path(id u) { return u ? nsstring_utf8(((URL *)u)->path) : NULL; }

// ---------------- NSNotificationCenter / NSNotification ----------------
typedef struct { Class_t *isa; id name, obj, info; } Note;
typedef struct { id observer; SEL sel; id name, obj; } Obs;
static Obs g_obs[128]; static int g_nobs;
static id note_new(id name, id obj, id info) { Note *n = (Note *)objc_alloc(C_Note, 0); n->name = objc_retain_(name); n->obj = obj; n->info = info; return (id)n; }
IMPL static id Note_name(Note *n, SEL s) { return n->name; }
IMPL static id Note_object(Note *n, SEL s) { return n->obj; }
IMPL static id Note_userInfo(Note *n, SEL s) { return n->info; }
IMPL static id Note_with(Class_t *c, SEL s, id name, id obj) { return objc_autorelease_(note_new(name, obj, NULL)); }
static id g_nc;
IMPL static id NC_default(Class_t *c, SEL s) { if (!g_nc) g_nc = objc_alloc(c, 0); return g_nc; }
IMPL static void NC_add(id nc, SEL s, id obs, SEL sel, id name, id obj) {
  if (g_nobs < 128) { g_obs[g_nobs++] = (Obs){obs, sel, objc_retain_(name), obj}; }
  dslogv("observer -[%s %s] for %s", class_name(obs->isa), sel, name ? nsstring_utf8(name) : "(todas)");
}
IMPL static void NC_remove(id nc, SEL s, id obs) { for (int i = 0; i < g_nobs; i++) if (g_obs[i].observer == obs) g_obs[i].observer = NULL; }
IMPL static void NC_removeNamed(id nc, SEL s, id obs, id name, id obj) {
  for (int i = 0; i < g_nobs; i++) if (g_obs[i].observer == obs && (!name || (g_obs[i].name && MSG(g_obs[i].name, "isEqual:", name)))) g_obs[i].observer = NULL;
}
void notify_post(id note) {
  Note *n = (Note *)note;
  for (int i = 0; i < g_nobs; i++) {
    Obs *o = &g_obs[i];
    if (!o->observer) continue;
    if (o->name && !MSG(o->name, "isEqual:", n->name)) continue;
    if (o->obj && o->obj != n->obj) continue;
    MSG(o->observer, o->sel, note);
  }
}
IMPL static void NC_post(id nc, SEL s, id note) { notify_post(note); }
IMPL static void NC_postNamed(id nc, SEL s, id name, id obj) { id n = note_new(name, obj, NULL); notify_post(n); objc_release_(n); }
IMPL static void NC_postNamedInfo(id nc, SEL s, id name, id obj, id info) { id n = note_new(name, obj, info); notify_post(n); objc_release_(n); }

// ---------------- NSDate ----------------
typedef struct { Class_t *isa; double t; } Date;   // seconds since 2001-01-01
double cf_now(void);
IMPL static double Date_refNow(Class_t *c, SEL s) { return cf_now(); }
IMPL static id Date_date(Class_t *c, SEL s) { Date *d = (Date *)objc_alloc(c, 0); d->t = cf_now(); return objc_autorelease_((id)d); }
IMPL static double Date_ti(Date *d, SEL s) { return d->t; }
IMPL static double Date_ti1970(Date *d, SEL s) { return d->t + 978307200.0; }
IMPL static double Date_sinceNow(Date *d, SEL s) { return d->t - cf_now(); }

IMPL static id Date_withRef(Class_t *c, SEL s, double t) { Date *d = (Date *)objc_alloc(c, 0); d->t = t; return objc_autorelease_((id)d); }
// NSCalendar / NSDateComponents (Gregorian calendar, Vita local time)
#include <time.h>
typedef struct { Class_t *isa; int y, mo, d, h, mi, s; } Comps;
static Class_t *C_Comps;
static id g_cal;
IMPL static id Cal_current(Class_t *c, SEL s) { if (!g_cal) g_cal = objc_alloc(c, 0); return g_cal; }
IMPL static id Cal_components(id cal, SEL s, uint32_t units, Date *date) {
  time_t t = (time_t)(date->t + 978307200.0); struct tm tm; localtime_r(&t, &tm);
  Comps *k = (Comps *)objc_alloc(C_Comps, 0);
  k->y = tm.tm_year + 1900; k->mo = tm.tm_mon + 1; k->d = tm.tm_mday; k->h = tm.tm_hour; k->mi = tm.tm_min; k->s = tm.tm_sec;
  return objc_autorelease_((id)k);
}
IMPL static int Comps_y(Comps *k, SEL s) { return k->y; }
IMPL static int Comps_mo(Comps *k, SEL s) { return k->mo; }
IMPL static int Comps_d(Comps *k, SEL s) { return k->d; }
IMPL static int Comps_h(Comps *k, SEL s) { return k->h; }
IMPL static int Comps_mi(Comps *k, SEL s) { return k->mi; }
IMPL static int Comps_s(Comps *k, SEL s) { return k->s; }

// ---------------- NSString constants the game imports as data ----------------
static struct { const char *sym; id val; } g_consts[] = {
  {"_NSFileHandleReadToEndOfFileCompletionNotification", NULL}, {"_NSFileHandleNotificationDataItem", NULL},
  {"_NSDefaultRunLoopMode", NULL}, {"_kCFRunLoopDefaultMode", NULL}, {"_NSGenericException", NULL},
  {"_NSLocalizedDescriptionKey", NULL}, {"_GCControllerDidConnectNotification", NULL}, {"_GCControllerDidDisconnectNotification", NULL},
  {"_kEAGLColorFormatRGBA8", NULL}, {"_kEAGLDrawablePropertyColorFormat", NULL}, {"_kEAGLDrawablePropertyRetainedBacking", NULL},
};
uintptr_t files_resolve_symbol(const char *name) {
  for (unsigned i = 0; i < sizeof(g_consts) / sizeof(g_consts[0]); i++)
    if (!strcmp(g_consts[i].sym, name)) {
      if (!g_consts[i].val) g_consts[i].val = nsstring_from_cstr(name + 1);
      return (uintptr_t)&g_consts[i].val;
    }
  if (!strcmp(name, "_NSSearchPathForDirectoriesInDomains")) return (uintptr_t)ios_NSSearchPathForDirectoriesInDomains;
  return 0;
}

// Validation of our own NSKeyedArchiver format (archive.c, "DSK1"): walks every entry with bounds checks.
static int v_obj(const uint8_t *b, uint32_t n, uint32_t *o, int depth);
static int v_entries(const uint8_t *b, uint32_t n, uint32_t o, uint32_t end, int depth) {
  while (o < end) {
    if (o + 2 > end) return 0; uint16_t kl; memcpy(&kl, b + o, 2); o += 2 + kl; if (o + 1 > end) return 0;
    char t = b[o++];
    if (t == 'i' || t == 'd' || t == 'b') { o += 8; if (o > end) return 0; }
    else if (t == 'o') { if (!v_obj(b, end, &o, depth + 1)) return 0; }
    else return 0;
  }
  return o == end;
}
static int v_obj(const uint8_t *b, uint32_t n, uint32_t *po, int depth) {
  uint32_t o = *po, c; if (depth > 64 || o + 1 > n) return 0;
  char t = b[o++];
  switch (t) {
    case 'n': break;
    case 'S': case 'D': if (o + 4 > n) return 0; memcpy(&c, b + o, 4); o += 4 + c; if (o > n) return 0; break;
    case 'I': case 'F': o += 8; if (o > n) return 0; break;
    case 'A': if (o + 4 > n) return 0; memcpy(&c, b + o, 4); o += 4; for (uint32_t i = 0; i < c; i++) if (!v_obj(b, n, &o, depth + 1)) return 0; break;
    case 'M': if (o + 4 > n) return 0; memcpy(&c, b + o, 4); o += 4; for (uint32_t i = 0; i < 2 * c; i++) if (!v_obj(b, n, &o, depth + 1)) return 0; break;
    case 'C': { if (o + 2 > n) return 0; uint16_t l; memcpy(&l, b + o, 2); o += 2 + l; if (o + 4 > n) return 0;
                memcpy(&c, b + o, 4); o += 4; if (o + c > n || !v_entries(b, n, o, o + c, depth + 1)) return 0; o += c; break; }
    default: return 0;
  }
  *po = o; return 1;
}
// 1 = sound (or not in our format but has data), 0 = missing or damaged
static int file_ok(const char *p) {
  SceUID fd = sceIoOpen(p, SCE_O_RDONLY, 0); if (fd < 0) return 0;
  int n = sceIoLseek(fd, 0, SCE_SEEK_END); sceIoLseek(fd, 0, SCE_SEEK_SET);
  if (n <= 0) { sceIoClose(fd); return 0; }
  uint8_t *b = malloc(n); int r = sceIoRead(fd, b, n); sceIoClose(fd);
  int ok = r == n;
  if (ok && n >= 4 && !memcmp(b, "DSK1", 4)) ok = v_entries(b, n, 4, n, 0);
  free(b); return ok;
}
static void copy_file(const char *a, const char *b) {
  SceUID i = sceIoOpen(a, SCE_O_RDONLY, 0); if (i < 0) return;
  SceUID o = sceIoOpen(b, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777); if (o < 0) { sceIoClose(i); return; }
  char buf[16384]; int r; while ((r = sceIoRead(i, buf, sizeof buf)) > 0) sceIoWrite(o, buf, r);
  sceIoClose(i); sceIoClose(o);
}
static void files_repair(void) {
  SceUID dd = sceIoDopen(DOC_DIR); if (dd < 0) return;
  SceIoDirent e; int fixed = 0, checked = 0;
  while (sceIoDread(dd, &e) > 0) {
    char *n = e.d_name; size_t l = strlen(n); char full[512], base[512];
    if (l > 4 && !strcmp(n + l - 4, ".tmp")) { snprintf(full, sizeof full, "%s/%s", DOC_DIR, n); sceIoRemove(full); continue; }
    if (l > 4 && !strcmp(n + l - 4, ".bak")) {
      snprintf(full, sizeof full, "%s/%s", DOC_DIR, n); snprintf(base, sizeof base, "%s/%.*s", DOC_DIR, (int)(l - 4), n);
      checked++;
      if (!file_ok(base) && file_ok(full)) { copy_file(full, base); fixed++; dslog("[error] save: %s was missing or damaged; restored from the .bak backup", base); }
    }
  }
  sceIoDclose(dd);
  dslog("save: %d backed-up files checked, %d restored", checked, fixed);
}
void files_init(void) {
  Class_t *O = g_cls_NSObject;
  g_note_read_done = nsstring_from_cstr("NSFileHandleReadToEndOfFileCompletionNotification");
  g_note_data_item = nsstring_from_cstr("NSFileHandleNotificationDataItem");
  g_consts[0].val = g_note_read_done; g_consts[1].val = g_note_data_item;
  sceIoMkdir(DOC_DIR, 0777);
  files_repair();
  { extern void archive_init(void); archive_init(); }
  int d = 0;
  if (!path_exists(APP_DIR "/data0000.bin", &d)) dslog("[RAM-bound] [error] game data missing in " APP_DIR " (copy the contents of deathsmiles.app there)");

  Class_t *B = objc_class_new("NSBundle", O, 4);
  class_add_meta(B, "mainBundle", Bundle_main);
  class_add(B, "bundlePath", Bundle_path); class_add(B, "resourcePath", Bundle_path);
  class_add(B, "pathForResource:ofType:", Bundle_pathFor); class_add(B, "infoDictionary", Bundle_info);
  class_add(B, "objectForInfoDictionaryKey:", Bundle_infoKey);
  class_add(B, "localizedStringForKey:value:table:", Bundle_localized); class_add(B, "bundleIdentifier", Bundle_ident);

  C_Data = objc_class_new("NSData", O, sizeof(Data)); C_MData = objc_class_new("NSMutableData", C_Data, sizeof(Data));
  class_add_meta(C_Data, "dataWithContentsOfFile:", Data_withFile); class_add_meta(C_Data, "dataWithBytes:length:", Data_withBytes);
  class_add_meta(C_Data, "dataWithLength:", Data_withLength); class_add_meta(C_Data, "data", Data_data);
  class_add_meta(C_Data, "dataWithCapacity:", Data_data);
  class_add(C_Data, "initWithContentsOfFile:", Data_initWithFile); class_add(C_Data, "init", Data_init);
  class_add(C_Data, "initWithCapacity:", Data_init);
  class_add(C_Data, "bytes", Data_bytes); class_add(C_Data, "mutableBytes", Data_bytes); class_add(C_Data, "length", Data_length);
  class_add(C_Data, "getBytes:length:", Data_getBytes); class_add(C_Data, "getBytes:", Data_getAll);
  class_add(C_Data, "getBytes:range:", Data_getRange); class_add(C_Data, "initWithBytes:length:", Data_initBytes);
  class_add(C_Data, "initWithBytesNoCopy:length:", Data_initBytesNoCopy);
  class_add(C_Data, "initWithBytesNoCopy:length:freeWhenDone:", Data_initBytesNoCopyFree);
  class_add_meta(C_Data, "dataWithBytesNoCopy:length:", Data_withBytesNoCopy);
  class_add_meta(C_Data, "dataWithBytesNoCopy:length:freeWhenDone:", Data_withBytesNoCopy);
  class_add(C_Data, "initWithLength:", Data_initLength); class_add(C_Data, "initWithData:", Data_initWithData);
  class_add_meta(C_Data, "dataWithData:", Data_withData); class_add(C_Data, "subdataWithRange:", Data_sub);
  class_add(C_Data, "isEqualToData:", Data_isEqual); class_add(C_Data, "isEqual:", Data_isEqual); class_add(C_Data, "writeToFile:atomically:", Data_write);
  class_add(C_Data, "dealloc", Data_dealloc); class_add(C_Data, "copy", Data_copy); class_add(C_Data, "mutableCopy", Data_mcopy);
  class_add(C_MData, "appendBytes:length:", Data_append); class_add(C_MData, "appendData:", Data_appendData);
  class_add(C_MData, "setLength:", Data_setLength);
  class_add(g_cls_NSString, "dataUsingEncoding:", Str_dataUsingEncoding);
  class_add(g_cls_NSString, "initWithData:encoding:", Str_initWithData);

  C_Handle = objc_class_new("NSFileHandle", O, sizeof(Handle));
  class_add_meta(C_Handle, "fileHandleForReadingAtPath:", Handle_forReading);
  class_add(C_Handle, "readDataToEndOfFile", Handle_readToEnd); class_add(C_Handle, "readDataOfLength:", Handle_readLen);
  class_add(C_Handle, "seekToFileOffset:", Handle_seek); class_add(C_Handle, "dealloc", Handle_dealloc); class_add(C_Handle, "closeFile", Handle_close);
  class_add(C_Handle, "readToEndOfFileInBackgroundAndNotify", Handle_readBg);

  Class_t *FM = objc_class_new("NSFileManager", O, 4);
  class_add_meta(FM, "defaultManager", FM_default);
  class_add(FM, "fileExistsAtPath:", FM_exists); class_add(FM, "fileExistsAtPath:isDirectory:", FM_existsDir);
  class_add(FM, "removeItemAtPath:error:", FM_remove); class_add(FM, "attributesOfItemAtPath:error:", FM_attrs);
  class_add(objc_getClass("NSDictionary"), "fileSize", Dict_fileSize);
  class_add(FM, "createDirectoryAtPath:withIntermediateDirectories:attributes:error:", FM_mkdir);

  C_URL = objc_class_new("NSURL", O, sizeof(URL));
  class_add_meta(C_URL, "fileURLWithPath:", URL_fileURL); class_add_meta(C_URL, "URLWithString:", URL_fileURL); class_add(C_URL, "path", URL_path);

  C_Note = objc_class_new("NSNotification", O, sizeof(Note));
  class_add_meta(C_Note, "notificationWithName:object:", Note_with);
  class_add(C_Note, "name", Note_name); class_add(C_Note, "object", Note_object); class_add(C_Note, "userInfo", Note_userInfo);
  Class_t *NC = objc_class_new("NSNotificationCenter", O, 4);
  class_add_meta(NC, "defaultCenter", NC_default);
  class_add(NC, "addObserver:selector:name:object:", NC_add); class_add(NC, "removeObserver:", NC_remove);
  class_add(NC, "removeObserver:name:object:", NC_removeNamed); class_add(NC, "postNotification:", NC_post);
  class_add(NC, "postNotificationName:object:", NC_postNamed); class_add(NC, "postNotificationName:object:userInfo:", NC_postNamedInfo);

  Class_t *D = objc_class_new("NSDate", O, sizeof(Date));
  class_add_meta(D, "timeIntervalSinceReferenceDate", Date_refNow); class_add_meta(D, "date", Date_date);
  class_add(D, "timeIntervalSinceReferenceDate", Date_ti); class_add(D, "timeIntervalSince1970", Date_ti1970);
  class_add(D, "timeIntervalSinceNow", Date_sinceNow);
  class_add_meta(D, "dateWithTimeIntervalSinceReferenceDate:", Date_withRef);
  Class_t *CAL = objc_getClass("NSCalendar"); if (!CAL) CAL = objc_class_new("NSCalendar", O, 4);
  class_add_meta(CAL, "currentCalendar", Cal_current); class_add(CAL, "components:fromDate:", Cal_components);
  C_Comps = objc_class_new("NSDateComponents", O, sizeof(Comps));
  class_add(C_Comps, "year", Comps_y); class_add(C_Comps, "month", Comps_mo); class_add(C_Comps, "day", Comps_d);
  class_add(C_Comps, "hour", Comps_h); class_add(C_Comps, "minute", Comps_mi); class_add(C_Comps, "second", Comps_s);
}
