// Assorted iOS system functions: blocks, dispatch, Darwin ctype, network, keychain, mach.
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "objc.h"

// ---------------- blocks (Apple ABI) ----------------
typedef struct { uint32_t reserved, size; void (*copy)(void *, const void *); void (*dispose)(const void *); } BlockDesc;
typedef struct { void *isa; int flags, reserved; void (*invoke)(void *, ...); BlockDesc *desc; } Block;
#define BLOCK_HAS_COPY_DISPOSE (1 << 25)
#define BLOCK_NEEDS_FREE (1 << 24)
static uint32_t g_malloc_block_isa[8], g_stack_block_isa[8], g_global_block_isa[8];
void *ios_Block_copy(const Block *b) {
  if (!b) return NULL;
  if (b->flags & BLOCK_NEEDS_FREE) { __atomic_add_fetch(&((Block *)b)->reserved, 1, __ATOMIC_RELAXED); return (void *)b; }
  if (b->isa == g_global_block_isa) return (void *)b;
  Block *n = malloc(b->desc->size);
  memcpy(n, b, b->desc->size);
  n->isa = g_malloc_block_isa; n->flags |= BLOCK_NEEDS_FREE; n->reserved = 1;
  if (b->flags & BLOCK_HAS_COPY_DISPOSE) b->desc->copy(n, b);
  return n;
}
void ios_Block_release(Block *b) {
  if (!b || !(b->flags & BLOCK_NEEDS_FREE)) return;
  if (__atomic_sub_fetch(&b->reserved, 1, __ATOMIC_ACQ_REL) == 0) {
    if (b->flags & BLOCK_HAS_COPY_DISPOSE) b->desc->dispose(b);
    free(b);
  }
}
static void ios_Block_object_assign(void **dst, const void *obj, int flags) {
  if ((flags & 0xff) == 7 || (flags & 0xff) == 3) *dst = (void *)obj;               // block or weak
  else if (flags & 8) *dst = (void *)obj;                                            // __block (byref): no copy
  else *dst = objc_retain_((id)obj);                                                 // object
}
static void ios_Block_object_dispose(const void *obj, int flags) { if (!(flags & 8) && (flags & 0xff) != 7) objc_release_((id)obj); }

// dispatch_async: the game only uses the main queue -> runs in the main thread loop
static uint32_t g_main_queue[16];
void block_invoke_and_release(id blk);
static void ios_dispatch_async(void *queue, Block *b) {
  Block *c = ios_Block_copy(b);
  if (queue != g_main_queue) dslog("[warning] dispatch_async to a queue other than the main one: run on the main thread");
  runloop_post(NULL, (SEL)"__block", (id)c);
}
void block_invoke_and_release(id blk) { Block *b = (Block *)blk; b->invoke(b); ios_Block_release(b); }

// ---------------- Darwin ctype (_DefaultRuneLocale / __maskrune) ----------------
#define CT_A 0x100
#define CT_C 0x200
#define CT_D 0x400
#define CT_G 0x800
#define CT_L 0x1000
#define CT_P 0x2000
#define CT_S 0x4000
#define CT_U 0x8000
#define CT_X 0x10000
#define CT_B 0x20000
#define CT_R 0x40000
static uint32_t rune_bits(int c) {
  uint32_t r = 0;
  if (c < 0 || c > 255) return 0;
  if (isalpha(c)) r |= CT_A; if (iscntrl(c)) r |= CT_C; if (isdigit(c)) r |= CT_D | 0;
  if (isgraph(c)) r |= CT_G; if (islower(c)) r |= CT_L; if (ispunct(c)) r |= CT_P; if (isspace(c)) r |= CT_S;
  if (isupper(c)) r |= CT_U; if (isxdigit(c)) r |= CT_X; if (c == ' ' || c == '\t') r |= CT_B; if (isprint(c)) r |= CT_R;
  return r;
}
static uint8_t g_rune_locale[52 + 256 * 4 * 3 + 64];
static void rune_init(void) {
  uint32_t *rt = (uint32_t *)(g_rune_locale + 52), *lo = rt + 256, *up = lo + 256;
  for (int c = 0; c < 128; c++) { rt[c] = rune_bits(c); lo[c] = tolower(c); up[c] = toupper(c); }
  for (int c = 128; c < 256; c++) { lo[c] = up[c] = c; }
}
static uint32_t ios_maskrune(int c, uint32_t mask) { return rune_bits(c) & mask; }

// ---------------- red, keychain, mach, CF varios ----------------
static void *ios_null(void) { return NULL; }
static int ios_zero(void) { return 0; }

// Reachability: the game asks "is there a network?" before using the in-app store (local StoreKit) and its
// re-download button. Wi-Fi is reported as connected (kSCNetworkReachabilityFlagsReachable); real connections
// still do not exist (NSURLConnection returns nil).
static int g_reach_dummy;
static void *ios_reach_create(void *a, void *b) { return &g_reach_dummy; }
static int ios_reach_flags(void *ref, uint32_t *flags) { if (flags) *flags = 2; return 1; }
static int ios_kern_failure(void) { return 5; }
static int ios_secItemNotFound(void) { return -25300; }
static void ios_CFRelease(id o) { objc_release_(o); }
static id ios_CFUUIDCreate(void) { return objc_alloc(g_cls_NSObject, 0); }
static id ios_CFUUIDCreateString(void *alloc, id uuid) { return nsstring_from_cstr("5D1E5D1E-0000-4000-8000-0000DEA7A5E5"); }
static void ios_noop(void) {}

uintptr_t misc_resolve_symbol(const char *name) {
  static const struct { const char *n; void *p; } t[] = {
    {"__Block_copy", ios_Block_copy}, {"__Block_object_assign", ios_Block_object_assign},
    {"__Block_object_dispose", ios_Block_object_dispose}, {"_dispatch_async", ios_dispatch_async},
    {"__NSConcreteStackBlock", g_stack_block_isa}, {"__dispatch_main_q", g_main_queue},
    {"__DefaultRuneLocale", g_rune_locale}, {"___maskrune", ios_maskrune},
    {"__Unwind_SjLj_Register", ios_noop}, {"__Unwind_SjLj_Unregister", ios_noop},
    {"__dyld_register_func_for_add_image", ios_noop},
    {"_SCNetworkReachabilityCreateWithAddress", ios_reach_create}, {"_SCNetworkReachabilityCreateWithName", ios_reach_create},
    {"_SCNetworkReachabilityGetFlags", ios_reach_flags}, {"_SCNetworkReachabilityScheduleWithRunLoop", ios_zero},
    {"_SCNetworkReachabilitySetCallback", ios_zero}, {"_SCNetworkReachabilityUnscheduleFromRunLoop", ios_zero},
    {"_SecItemAdd", ios_secItemNotFound}, {"_SecItemCopyMatching", ios_secItemNotFound}, {"_SecItemDelete", ios_secItemNotFound},
    {"_host_statistics", ios_kern_failure}, {"_task_info", ios_kern_failure}, {"_mach_host_self", ios_zero},
    {"_CFRelease", ios_CFRelease}, {"_CFUUIDCreate", ios_CFUUIDCreate}, {"_CFUUIDCreateString", ios_CFUUIDCreateString},
    {"_CFRunLoopGetCurrent", ios_null},
  };
  for (unsigned i = 0; i < sizeof(t) / sizeof(t[0]); i++) if (!strcmp(t[i].n, name)) return (uintptr_t)t[i].p;
  return 0;
}
void misc_init(void) { rune_init(); }
