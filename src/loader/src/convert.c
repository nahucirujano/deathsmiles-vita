// First-launch conversion of the iOS executable into game.bin (C port of tools/macho_prep.py).
// The user only copies the contents of deathsmiles.app to ux0:data/deathsmiles/app/; on the first launch the
// loader takes app/deathsmiles (a fat Mach-O with armv7 and armv7s slices), extracts the armv7 slice and writes
// ux0:data/deathsmiles/game.bin in the format load_game() expects. The result is byte-identical to macho_prep.py.
// Builds on the Vita and on a PC (gcc -DCONVERT_TEST) so the output can be compared with the Python tool.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define MAIN_ADDR 0x14499u           // main() (Thumb) in Deathsmiles 1.0.9, found by disassembly
#define TEXT_CRC  0xb881c351u        // CRC-32 of __TEXT,__text of the supported (decrypted) 1.0.9 armv7 slice

static uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint32_t rd32be(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
static uint32_t crc32_buf(const uint8_t *p, uint32_t n) {
  uint32_t c = 0xffffffffu;
  for (uint32_t i = 0; i < n; i++) { c ^= p[i]; for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xedb88320u & -(c & 1)); }
  return ~c;
}
static uint32_t uleb(const uint8_t *b, uint32_t *i) { uint32_t r = 0; int s = 0; uint8_t x; do { x = b[(*i)++]; if (s < 32) r |= (uint32_t)(x & 0x7f) << s; s += 7; } while (x & 0x80); return r; }
static int32_t sleb(const uint8_t *b, uint32_t *i) { int32_t r = 0; int s = 0; uint8_t x; do { x = b[(*i)++]; if (s < 32) r |= (int32_t)(x & 0x7f) << s; s += 7; } while (x & 0x80); if ((x & 0x40) && s < 32) r |= -(1 << s); return r; }

typedef struct { uint32_t va; int32_t add; const char *sym; int weak; } Imp;
typedef struct { uint32_t *v; uint32_t n, cap; } Vec;
static void vpush(Vec *v, uint32_t x) { if (v->n == v->cap) { v->cap = v->cap ? v->cap * 2 : 1024; v->v = realloc(v->v, v->cap * 4); } v->v[v->n++] = x; }
static Imp *g_imp; static uint32_t g_nimp, g_capimp;
static void ipush(uint32_t va, int32_t add, const char *sym, int weak) {
  if (g_nimp == g_capimp) { g_capimp = g_capimp ? g_capimp * 2 : 1024; g_imp = realloc(g_imp, g_capimp * sizeof(Imp)); }
  g_imp[g_nimp++] = (Imp){va, add, sym, weak};
}
static int starts(const char *s, const char *p) { return !strncmp(s, p, strlen(p)); }
static int ends(const char *s, const char *p) { size_t a = strlen(s), b = strlen(p); return a >= b && !strcmp(s + a - b, p); }
static int is_data(const char *n) {
  static const char *exact[] = {"__NSConcreteStackBlock", "__NSConcreteGlobalBlock", "__DefaultRuneLocale", "_vm_page_size", "_mach_task_self_",
    "__dispatch_main_q", "_NSGenericException", "__objc_empty_cache", "__objc_empty_vtable", "_NXArgc", "_NXArgv", "_environ", "___progname"};
  static const char *pre[] = {"_OBJC_CLASS_$_", "_OBJC_METACLASS_$_", "__NSConcrete", "_k", "___stack_chk_guard", "___stderrp", "___stdoutp",
    "___stdinp", "_NSLocalizedDescriptionKey", "___CFConstantStringClassReference"};
  for (unsigned i = 0; i < sizeof exact / sizeof *exact; i++) if (!strcmp(n, exact[i])) return 1;
  if ((starts(n, "_NS") || starts(n, "_UI") || starts(n, "_GC")) && !ends(n, "Log") && !ends(n, "Main") &&
      (strstr(n, "Notification") || strstr(n, "Key") || strstr(n, "Name") || strstr(n, "Mode"))) return 1;
  for (unsigned i = 0; i < sizeof pre / sizeof *pre; i++) if (starts(n, pre[i])) return 1;
  return 0;
}
static int cmp_str(const void *a, const void *b) { return strcmp(*(const char *const *)a, *(const char *const *)b); }

// Returns 0 on success; on failure fills err (English, shown to the user) and returns <0.
int convert_game(const char *src, const char *dst, char *err, int errlen) {
  FILE *f = fopen(src, "rb");
  if (!f) { snprintf(err, errlen, "Game executable not found: %s\nCopy the contents of deathsmiles.app to ux0:data/deathsmiles/app/", src); return -1; }
  fseek(f, 0, SEEK_END); long fsz = ftell(f); fseek(f, 0, SEEK_SET);
  uint8_t *file = malloc(fsz); if (!file || fread(file, 1, fsz, f) != (size_t)fsz) { fclose(f); free(file); snprintf(err, errlen, "Could not read %s", src); return -2; }
  fclose(f);
  const uint8_t *d = file; uint32_t dsz = fsz;
  if (rd32be(file) == 0xcafebabe) {                       // fat binary: pick the armv7 slice (CPU 12, subtype 9)
    uint32_t n = rd32be(file + 4); d = NULL;
    for (uint32_t i = 0; i < n; i++) {
      const uint8_t *a = file + 8 + 20 * i;
      if (rd32be(a) == 12 && rd32be(a + 4) == 9) { d = file + rd32be(a + 8); dsz = rd32be(a + 12); }
    }
    if (!d) { free(file); snprintf(err, errlen, "The executable has no armv7 code."); return -3; }
  }
  (void)dsz;
  if (rd32(d) != 0xfeedface) { free(file); snprintf(err, errlen, "Not a 32-bit iOS executable."); return -3; }
  uint32_t ncmds = rd32(d + 16), off = 28;
  struct { char name[17]; uint32_t va, vs, fo, fs; } segs[16]; int nseg = 0;
  struct { char seg[17], sect[17]; uint32_t a, s, fo; } sects[96]; int nsect = 0;
  const uint8_t *dyld = NULL; uint32_t cryptid = 0;
  for (uint32_t c = 0; c < ncmds; c++) {
    uint32_t cmd = rd32(d + off), size = rd32(d + off + 4);
    if (cmd == 1 && nseg < 16) {
      memcpy(segs[nseg].name, d + off + 8, 16); segs[nseg].name[16] = 0;
      segs[nseg].va = rd32(d + off + 24); segs[nseg].vs = rd32(d + off + 28); segs[nseg].fo = rd32(d + off + 32); segs[nseg].fs = rd32(d + off + 36);
      uint32_t ns = rd32(d + off + 48), so = off + 56;
      for (uint32_t k = 0; k < ns && nsect < 96; k++, so += 68) {
        memcpy(sects[nsect].sect, d + so, 16); sects[nsect].sect[16] = 0; memcpy(sects[nsect].seg, d + so + 16, 16); sects[nsect].seg[16] = 0;
        sects[nsect].a = rd32(d + so + 32); sects[nsect].s = rd32(d + so + 36); sects[nsect].fo = rd32(d + so + 40); nsect++;
      }
      nseg++;
    } else if (cmd == 0x80000022) dyld = d + off + 8;
    else if (cmd == 0x21) cryptid = rd32(d + off + 16);
    off += size;
  }
  if (cryptid) { free(file); snprintf(err, errlen, "This game executable is encrypted (App Store DRM) and cannot run on the Vita.\nA decrypted copy of Deathsmiles 1.0.9 is required."); return -4; }
  for (int k = 0; k < nsect; k++)
    if (!strcmp(sects[k].seg, "__TEXT") && !strcmp(sects[k].sect, "__text") && crc32_buf(d + sects[k].fo, sects[k].s) != TEXT_CRC) {
      free(file); snprintf(err, errlen, "Unsupported game version. This port requires Deathsmiles 1.0.9 for iOS."); return -5;
    }
  if (!dyld) { free(file); snprintf(err, errlen, "Unexpected executable format (no dyld info)."); return -3; }
  uint32_t vmbase = 0xffffffff, vmend = 0, imgend = 0;
  for (int k = 0; k < nseg; k++) {
    if (!strcmp(segs[k].name, "__PAGEZERO") || !strcmp(segs[k].name, "__LINKEDIT")) continue;
    if (segs[k].va < vmbase) vmbase = segs[k].va;
    if (segs[k].va + segs[k].vs > vmend) vmend = segs[k].va + segs[k].vs;
    if (segs[k].va + segs[k].fs > imgend) imgend = segs[k].va + segs[k].fs;
  }
  uint32_t imgsz = imgend - vmbase;
  uint8_t *img = calloc(1, imgsz);
  for (int k = 0; k < nseg; k++) {
    if (!strcmp(segs[k].name, "__PAGEZERO") || !strcmp(segs[k].name, "__LINKEDIT")) continue;
    memcpy(img + segs[k].va - vmbase, d + segs[k].fo, segs[k].fs);
  }
  // rebases
  Vec rel = {0}; { const uint8_t *b = d + rd32(dyld); uint32_t bs = rd32(dyld + 4), i = 0, seg = 0, o = 0;
    while (i < bs) {
      uint8_t x = b[i++], op = x & 0xf0, imm = x & 0x0f;
      if (op == 0x00) break;
      else if (op == 0x10) ;
      else if (op == 0x20) { seg = imm; o = uleb(b, &i); }
      else if (op == 0x30) o += uleb(b, &i);
      else if (op == 0x40) o += imm * 4;
      else if (op == 0x50) for (int r = 0; r < imm; r++) { vpush(&rel, segs[seg].va + o); o += 4; }
      else if (op == 0x60) { uint32_t n = uleb(b, &i); for (uint32_t r = 0; r < n; r++) { vpush(&rel, segs[seg].va + o); o += 4; } }
      else if (op == 0x70) { vpush(&rel, segs[seg].va + o); o += 4 + uleb(b, &i); }
      else if (op == 0x80) { uint32_t n = uleb(b, &i), sk = uleb(b, &i); for (uint32_t r = 0; r < n; r++) { vpush(&rel, segs[seg].va + o); o += 4 + sk; } }
      else { free(file); free(img); snprintf(err, errlen, "Unknown rebase opcode %x", op); return -6; }
    } }
  // binds (normal, weak, lazy)
  for (int pass = 0; pass < 3; pass++) {
    const uint8_t *b = d + rd32(dyld + 8 + pass * 8); uint32_t bs = rd32(dyld + 12 + pass * 8), i = 0, seg = 0, o = 0; int32_t add = 0;
    const char *sym = ""; int lazy = pass == 2, weak = pass == 1;
    while (i < bs) {
      uint8_t x = b[i++], op = x & 0xf0, imm = x & 0x0f;
      if (op == 0x00) { if (lazy) continue; break; }
      if (op == 0x10 || op == 0x30 || op == 0x50) ;
      else if (op == 0x20) uleb(b, &i);
      else if (op == 0x40) { sym = (const char *)b + i; i += strlen(sym) + 1; }
      else if (op == 0x60) add = sleb(b, &i);
      else if (op == 0x70) { seg = imm; o = uleb(b, &i); }
      else if (op == 0x80) o += uleb(b, &i);
      else if (op == 0x90) { ipush(segs[seg].va + o, add, sym, weak); o += 4; }
      else if (op == 0xa0) { ipush(segs[seg].va + o, add, sym, weak); o += 4 + uleb(b, &i); }
      else if (op == 0xb0) { ipush(segs[seg].va + o, add, sym, weak); o += 4 + imm * 4; }
      else if (op == 0xc0) { uint32_t n = uleb(b, &i), sk = uleb(b, &i); for (uint32_t r = 0; r < n; r++) { ipush(segs[seg].va + o, add, sym, weak); o += 4 + sk; } }
      else { free(file); free(img); snprintf(err, errlen, "Unknown bind opcode %x", op); return -6; }
    }
  }
  // initializers
  Vec ini = {0};
  for (int k = 0; k < nsect; k++) if (!strcmp(sects[k].seg, "__DATA") && !strcmp(sects[k].sect, "__mod_init_func"))
    for (uint32_t q = 0; q < sects[k].s; q += 4) vpush(&ini, rd32(img + sects[k].a - vmbase + q));
  // sorted unique symbol names → string table
  const char **names = malloc(g_nimp * sizeof(char *)); uint32_t nn = 0;
  for (uint32_t k = 0; k < g_nimp; k++) names[nn++] = g_imp[k].sym;
  qsort(names, nn, sizeof(char *), cmp_str);
  uint32_t nu = 0; for (uint32_t k = 0; k < nn; k++) if (!nu || strcmp(names[k], names[nu - 1])) names[nu++] = names[k];
  uint32_t *stro = malloc(nu * 4), stsz = 0;
  for (uint32_t k = 0; k < nu; k++) { stro[k] = stsz; stsz += strlen(names[k]) + 1; }
  static const char *want[] = {"__objc_classlist", "__objc_nlclslist", "__objc_catlist", "__objc_selrefs", "__objc_classrefs",
    "__objc_superrefs", "__cfstring", "__objc_protolist", "__objc_imageinfo", "__program_vars"};
  int sidx[10], nsec = 0;
  for (int w = 0; w < 10; w++) for (int k = 0; k < nsect; k++) if (!strcmp(sects[k].seg, "__DATA") && !strcmp(sects[k].sect, want[w])) { sidx[nsec++] = k; break; }
  // write
  FILE *o = fopen(dst, "wb");
  if (!o) { free(file); free(img); snprintf(err, errlen, "Could not write %s", dst); return -7; }
  uint32_t hdr[11] = {0, 2, vmbase, imgsz, vmend - vmbase, MAIN_ADDR, ini.n, rel.n, g_nimp, stsz, (uint32_t)nsec};
  memcpy(hdr, "DSLD", 4); fwrite(hdr, 4, 11, o);
  for (int k = 0; k < nsec; k++) { char nm[16] = {0}; strncpy(nm, sects[sidx[k]].sect, 16); fwrite(nm, 1, 16, o); fwrite(&sects[sidx[k]].a, 4, 1, o); fwrite(&sects[sidx[k]].s, 4, 1, o); }
  fwrite(ini.v, 4, ini.n, o); fwrite(rel.v, 4, rel.n, o);
  for (uint32_t k = 0; k < g_nimp; k++) {
    const char **p = bsearch(&g_imp[k].sym, names, nu, sizeof(char *), cmp_str);
    uint32_t e[4] = {g_imp[k].va, (uint32_t)g_imp[k].add, stro[p - names], (uint32_t)(is_data(g_imp[k].sym) | (g_imp[k].weak ? 2 : 0))};
    fwrite(e, 4, 4, o);
  }
  for (uint32_t k = 0; k < nu; k++) fwrite(names[k], 1, strlen(names[k]) + 1, o);
  fwrite(img, 1, imgsz, o);
  int bad = ferror(o); fclose(o);
  free(names); free(stro); free(ini.v); free(rel.v); free(g_imp); g_imp = NULL; g_nimp = g_capimp = 0; free(img); free(file);
  if (bad) { remove(dst); snprintf(err, errlen, "Write error on %s (memory card full?)", dst); return -7; }
  return 0;
}

#ifdef CONVERT_TEST
int main(int argc, char **argv) { char e[256]; int r = convert_game(argv[1], argv[2], e, sizeof e); if (r) printf("error %d: %s\n", r, e); return r; }
#endif
