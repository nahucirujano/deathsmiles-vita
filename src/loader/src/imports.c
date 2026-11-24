// Table of the symbols the iOS executable imports that are implemented for the Vita.
// Calling convention: iOS armv7 passes float/double in integer registers (like softfp),
// so every function with float/double exposed to the game is marked SOFTFP.
#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/processmgr.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <stdarg.h>
#include "ds.h"

// ---------- libc / runtime ----------
uintptr_t ios_stack_chk_guard = 0x5d5e5f60;
uint32_t ios_vm_page_size = 4096;
uint32_t ios_mach_task_self = 0x103;
static void ios_stack_chk_fail(void) { dslog("[CPU-bound] [error] stack_chk_fail: stack corruption"); for (;;) sceKernelDelayThread(1000000); }
static void ios_assert_rtn(const char *fn, const char *file, int line, const char *expr) {
  dslog("[error] assert %s in %s:%d (%s)", expr ? expr : "?", file ? file : "?", line, fn ? fn : "?");
}
static void ios_exit(int c) { dslog("exit(%d) called by the game", c); dslog_flush(); for (;;) sceKernelDelayThread(1000000); }
static void ios_memset_pattern16(void *b, const void *pat, size_t len) {
  uint8_t *p = b; for (size_t i = 0; i < len; i++) p[i] = ((const uint8_t *)pat)[i & 15];
}
static char *ios_strnstr(const char *s, const char *find, size_t slen) {
  size_t n = strlen(find); if (!n) return (char *)s;
  for (size_t i = 0; i + n <= slen && s[i]; i++) if (!strncmp(s + i, find, n)) return (char *)s + i;
  return NULL;
}
char *ns_vformat(const char *fmt, va_list ap);
// the game's sprintf with the iOS ABI (64-bit values 4-byte aligned): same formatter as NSString
static int ios_sprintf(char *s, const char *fmt, ...) {
  va_list ap; va_start(ap, fmt); char *t = ns_vformat(fmt, ap); va_end(ap);
  int r = strlen(t); memcpy(s, t, r + 1); free(t); return r;
}
static int ios_divsi3(int a, int b) { return b ? a / b : 0; }
static int ios_modsi3(int a, int b) { return b ? a % b : 0; }
static unsigned ios_udivsi3(unsigned a, unsigned b) { return b ? a / b : 0; }
static unsigned ios_umodsi3(unsigned a, unsigned b) { return b ? a % b : 0; }
static uint64_t ios_udivdi3(uint64_t a, uint64_t b) { return b ? a / b : 0; }
static uint64_t ios_umoddi3(uint64_t a, uint64_t b) { return b ? a % b : 0; }
static unsigned ios_sleep(unsigned s) { sceKernelDelayThread(s * 1000000); return 0; }
static int ios_usleep(unsigned us) { sceKernelDelayThread(us); return 0; }

// ---------- math (softfp) ----------
SOFTFP static double ios_sin(double x) { return sin(x); }
SOFTFP static double ios_cos(double x) { return cos(x); }
SOFTFP static double ios_atan2(double y, double x) { return atan2(y, x); }
SOFTFP static double ios_floor(double x) { return floor(x); }
SOFTFP static double ios_trunc(double x) { return trunc(x); }
SOFTFP static float ios_floorf(float x) { return floorf(x); }
SOFTFP static float ios_ceilf(float x) { return ceilf(x); }
SOFTFP static float ios_truncf(float x) { return truncf(x); }
SOFTFP static uint64_t ios_fixunsdfdi(double x) { return (uint64_t)x; }
SOFTFP static float ios_floatundisf(uint64_t x) { return (float)x; }

typedef struct { const char *name; void *ptr; } Sym;
static const Sym g_syms[] = {
  {"___stack_chk_guard", &ios_stack_chk_guard}, {"___stack_chk_fail", ios_stack_chk_fail},
  {"_vm_page_size", &ios_vm_page_size}, {"_mach_task_self_", &ios_mach_task_self},
  {"___assert_rtn", ios_assert_rtn}, {"_exit", ios_exit},
  {"_malloc", malloc}, {"_calloc", calloc}, {"_free", free},
  {"_memcpy", memcpy}, {"_memmove", memmove}, {"_memset", memset}, {"_bzero", bzero},
  {"_memset_pattern16", ios_memset_pattern16},
  {"_strlen", strlen}, {"_strcmp", strcmp}, {"_strncmp", strncmp}, {"_strnstr", ios_strnstr},
  {"_sprintf", ios_sprintf}, {"_rand", rand}, {"_srand", srand},
  {"_sleep", ios_sleep}, {"_usleep", ios_usleep},
  {"___divsi3", ios_divsi3}, {"___modsi3", ios_modsi3}, {"___udivsi3", ios_udivsi3}, {"___umodsi3", ios_umodsi3},
  {"___udivdi3", ios_udivdi3}, {"___umoddi3", ios_umoddi3},
  {"_sin", ios_sin}, {"_cos", ios_cos}, {"_atan2", ios_atan2}, {"_floor", ios_floor}, {"_trunc", ios_trunc},
  {"_floorf", ios_floorf}, {"_ceilf", ios_ceilf}, {"_truncf", ios_truncf},
  {"___fixunsdfdi", ios_fixunsdfdi}, {"___floatundisf", ios_floatundisf},
};

uintptr_t resolve_import(const char *name, int is_data) {
  uintptr_t o = objc_resolve_symbol(name);
  if (o) return o;
  for (unsigned i = 0; i < sizeof(g_syms) / sizeof(g_syms[0]); i++)
    if (!strcmp(g_syms[i].name, name)) return (uintptr_t)g_syms[i].ptr;
  return 0;
}

// Unimplemented data: a zero-filled block per symbol (logged).
typedef struct DD { const char *name; void *p; struct DD *next; } DD;
static DD *g_dd;
void *dummy_data(const char *name) {
  for (DD *d = g_dd; d; d = d->next) if (!strcmp(d->name, name)) return d->p;
  DD *d = malloc(sizeof(DD)); d->name = name; d->p = calloc(1, 4096); d->next = g_dd; g_dd = d;
  dslog("[not-implemented] data %s -> empty block", name);
  return d->p;
}
const GameSection *game_section(const char *name) {
  for (uint32_t i = 0; i < g_nsections; i++) if (!strncmp(g_sections[i].name, name, 16)) return &g_sections[i];
  return NULL;
}
