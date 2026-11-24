// Deathsmiles iOS -> PS Vita: loader for the armv7 executable (converted to game.bin by convert.c / macho_prep.py)
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/io/fcntl.h>
#include <kubridge.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "ds.h"

uintptr_t g_base;      // actual address where the image was mapped
uint32_t  g_vmbase;    // original address (0x4000)
uint32_t  g_vmsize;
static const char *g_strtab;
GameSection g_sections[16]; uint32_t g_nsections;
static uint32_t g_nfunc_missing;
static const char **g_thunk_name;
static uint32_t *g_thunk_hits;

// ---- trampoline for unimplemented functions: r12 = index ----
uint32_t missing_c(uint32_t idx, uint32_t a0, uint32_t a1, uint32_t lr) {
  if (g_thunk_hits[idx]++ < 3)
    dslog("[CPU-bound] [not-implemented] %s(r0=%08x r1=%08x) from game+%#x",
          g_thunk_name[idx], a0, a1, (unsigned)(lr - g_base + g_vmbase));
  return 0;
}
// iOS armv7 only guarantees a 4-byte aligned stack; our code (AAPCS) assumes 8 and uses aligned NEON accesses
// on the stack. That is why every entry point called from the game realigns the stack.
__asm__(
  ".arm\n .align 2\n .global missing_entry\n .type missing_entry, %function\n missing_entry:\n"
  "  push {r4, lr}\n  mov r4, sp\n"
  "  mov r3, lr\n  mov r2, r1\n  mov r1, r0\n  mov r0, r12\n"
  "  mov r12, sp\n  bic r12, r12, #7\n  mov sp, r12\n"
  "  bl missing_c\n"
  "  mov sp, r4\n  mov r1, #0\n  pop {r4, pc}\n"
  ".thumb\n");
void missing_entry(void);
// realign_call: r12 = target. If the stack is misaligned, copies 24 words of stack arguments
// to an aligned area and calls the target; if it is already aligned, jumps straight to it.
__asm__(
  ".arm\n .align 2\n .global realign_call\n .type realign_call, %function\n realign_call:\n"
  "  tst sp, #4\n  bxeq r12\n"
  "  push {r4, r5, r6, lr}\n"
  "  add r4, sp, #16\n  mov r5, sp\n"
  "  sub sp, sp, #96\n  bic sp, sp, #7\n"
  "  mov r6, #0\n"
  "1: ldr lr, [r4, r6]\n  str lr, [sp, r6]\n  add r6, r6, #4\n  cmp r6, #96\n  blt 1b\n"
  "  blx r12\n"
  "  mov sp, r5\n  pop {r4, r5, r6, pc}\n"
  ".thumb\n");
void realign_call(void);

static void *read_file(const char *path, uint32_t *size) {
  SceUID fd = sceIoOpen(path, SCE_O_RDONLY, 0);
  if (fd < 0) return NULL;
  uint32_t sz = sceIoLseek(fd, 0, SCE_SEEK_END); sceIoLseek(fd, 0, SCE_SEEK_SET);
  void *buf = malloc(sz);
  if (buf && sceIoRead(fd, buf, sz) != (int)sz) { free(buf); buf = NULL; }
  sceIoClose(fd); *size = sz; return buf;
}

int load_game(const char *path, uint32_t *main_addr) {
  uint32_t fsz;
  uint8_t *f = read_file(path, &fsz);
  if (!f) { dslog("[RAM-bound] [error] could not read %s", path); return -1; }
  uint32_t *h = (uint32_t *)f;
  if (memcmp(f, "DSLD", 4) || h[1] != 2) { dslog("[error] invalid game.bin or a different version"); return -2; }
  g_vmbase = h[2]; uint32_t imgsize = h[3]; g_vmsize = h[4];
  uint32_t entry = h[5], ninit = h[6], nreloc = h[7], nimp = h[8], strsz = h[9], nsec = h[10];
  const GameSection *secs = (const GameSection *)(h + 11);
  uint32_t *inits = (uint32_t *)(secs + nsec), *relocs = inits + ninit;
  uint32_t *imps = relocs + nreloc;                   // 4 words per import
  g_strtab = (const char *)(imps + 4 * nimp);
  uint8_t *img = (uint8_t *)g_strtab + strsz;

  // image + trampoline area (16 bytes per import) + dummy data
  uint32_t thunk_off = (g_vmsize + 0xfff) & ~0xfff;
  uint32_t total = (thunk_off + nimp * 16 + 0xfffff) & ~0xfffff;
  SceKernelAllocMemBlockKernelOpt opt;            // kubridge requires a non-null opt (it copies it to the kernel)
  memset(&opt, 0, sizeof(opt)); opt.size = sizeof(opt);
  SceUID blk = kuKernelAllocMemBlock("ds_game", 0x0C20D050 /* USER_RX */, total, &opt);
  if (blk < 0) { dslog("[RAM-bound] [error] kuKernelAllocMemBlock=%08x ", blk); return -3; }
  void *base; sceKernelGetMemBlockBase(blk, &base);
  int r = kuKernelMemProtect(base, total, KU_KERNEL_PROT_READ | KU_KERNEL_PROT_WRITE | KU_KERNEL_PROT_EXEC);
  if (r < 0) { dslog("[error] kuKernelMemProtect=%08x", r); return -4; }
  g_base = (uintptr_t)base;
  uint32_t slide = g_base - g_vmbase;
  memset(base, 0, g_vmsize);
  memcpy(base, img, imgsize);
  dslog("[RAM-bound] image %u KiB loaded at %p (slide %#x), block %u KiB", g_vmsize >> 10, base, slide, total >> 10);

  for (uint32_t i = 0; i < nreloc; i++) *(uint32_t *)(relocs[i] + slide) += slide;

  objc_bridge_init();                               // Apple classes imported by the executable
  uint32_t *thunks = (uint32_t *)(g_base + thunk_off);
  g_thunk_name = calloc(nimp, sizeof(char *));
  g_thunk_hits = calloc(nimp, sizeof(uint32_t));
  uint32_t nfunc = 0, ndata = 0, nok = 0, nweak = 0;
  for (uint32_t i = 0; i < nimp; i++) {
    uint32_t va = imps[4 * i], name_off = imps[4 * i + 2], flags = imps[4 * i + 3];
    int32_t add = (int32_t)imps[4 * i + 1];
    const char *name = g_strtab + name_off;
    uintptr_t target = resolve_import(name, flags & 1);
    if (!target) {
      if (flags & 2) { nweak++; continue; }        // weak def: the executable's own definition is kept
      if (flags & 1) { target = (uintptr_t)dummy_data(name); ndata++; }
      else {
        uint32_t *t = thunks + 4 * i;
        t[0] = 0xe59fc000; t[1] = 0xe59ff000; t[2] = i; t[3] = (uint32_t)missing_entry;
        g_thunk_name[i] = name; target = (uintptr_t)t; nfunc++;
      }
    } else {
      nok++;
      if (!(flags & 1) && strncmp(name, "_objc_msgSend", 13)) {   // one of our functions: go through realign_call
        uint32_t *t = thunks + 4 * i;
        t[0] = 0xe59fc000; t[1] = 0xe59ff000; t[2] = target; t[3] = (uint32_t)realign_call;
        target = (uintptr_t)t;
      }
    }
    *(uint32_t *)(va + slide) = target + add;
  }
  g_nfunc_missing = nfunc;
  // ---- targeted patches to the game code ----
  // game+0x18b10: sprite allocation function (0x18af4) called with layer -1 (the game does this when some stages
  // end, e.g. after Mary in C-1). It reads counter[-1], which at that memory position is a pointer. On iOS,
  // pointers are < 0x80000000 and the signed comparison "counter > 5000" took the "nil" path; on the Vita pointers
  // are 0x8xxxxxxx (negative when signed), so it continued and read outside the table -> crash. "ITT GT" is changed
  // to "ITT HI" (unsigned comparison): same result as on iOS.
  { uint16_t *p = (uint16_t *)(0x18b10 + slide);
    if (*p == 0xBFC4) { *p = 0xBF84; dslog("patch: game+0x18b10 ITT GT -> ITT HI (sprite layer -1)"); }
    else dslog("[warning] patch game+0x18b10: unexpected instruction %04x, not applied", *p); }
  kuKernelFlushCaches(base, total);
  dslog("imports: %u resolved, %u unimplemented functions, %u dummy data, %u weak", nok, nfunc, ndata, nweak);
  for (uint32_t i = 0; i < nsec; i++) { g_sections[i] = secs[i]; g_sections[i].addr += slide; }
  g_nsections = nsec;
  objc_load_image();                                 // the game's classes, categories and selectors

  for (uint32_t i = 0; i < ninit; i++) {
    dslog("calling initializer %u at game+%#x", i, inits[i]);
    ((void (*)(int, char **, char **, char **))(inits[i] + slide))(0, NULL, NULL, NULL);
  }
  *main_addr = entry + slide;
  return 0;
}
