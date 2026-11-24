#include <stdio.h>
// Deathsmiles for PS Vita: loads the iOS executable (game.bin) and runs its main().
#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/ctrl.h>
#include <kubridge.h>
#include <string.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include "ds.h"
int convert_game(const char *src, const char *dst, char *err, int errlen);
#include "tstate.h"

int _newlib_heap_size_user = 128 * 1024 * 1024;   // heap for the game (the iPhone 3GS had 256 MB in total)

static void on_exception(KuKernelExceptionContext *c) {
  static const char *tipo[] = {"data abort", "prefetch abort", "undefined instruction"};
  int in_game = c->pc >= g_base && c->pc < g_base + g_vmsize;
  SceKernelThreadInfo ti = {sizeof(ti)};
  sceKernelGetThreadInfo(sceKernelGetThreadId(), &ti);
  dslog("[crash] thread '%s' stack %p-%p", ti.name, ti.stack, (char *)ti.stack + ti.stackSize);
  dslog("[CPU-bound] [crash] %s: pc=%s%#x lr=game+%#x FAR=%08x sp=%08x",
        c->exceptionType < 3 ? tipo[c->exceptionType] : "?", in_game ? "game+" : "",
        in_game ? GAMEADDR(c->pc) : (unsigned)c->pc, GAMEADDR(c->lr), (unsigned)c->FAR, (unsigned)c->sp);
  dslog("r0=%08x r1=%08x r2=%08x r3=%08x r4=%08x r5=%08x r6=%08x r7=%08x",
        (unsigned)c->r0, (unsigned)c->r1, (unsigned)c->r2, (unsigned)c->r3, (unsigned)c->r4, (unsigned)c->r5, (unsigned)c->r6, (unsigned)c->r7);
  dslog("eboot: on_exception at %p (to locate pc/lr in the ELF)", (void *)on_exception);
  { char buf[400]; int o = 0; uint32_t *sp = (uint32_t *)c->sp;   // game return addresses on the stack
    for (int i = 0; i < 128 && o < 360; i++) { uint32_t w = sp[i]; if (w >= g_base && w < g_base + g_vmsize && (w & 1)) o += snprintf(buf + o, sizeof(buf) - o, "game+%x ", GAMEADDR(w)); }
    dslog("stack (game return addresses): %s", o ? buf : "(ninguno)"); }
  dslog("Please send a photo of this screen or the file ux0:data/deathsmiles/log.txt. Press START to exit.");
  dslog_flush();
  for (;;) {
    SceCtrlData pad; sceCtrlPeekBufferPositive(0, &pad, 1);
    if (pad.buttons & SCE_CTRL_START) { dslog_flush(); sceKernelExitProcess(0); }
    sceKernelDelayThread(50000);
  }
}

static int game_thread(SceSize args, void *argp) {
  STACK_PAD();
  uint32_t main_addr = *(uint32_t *)argp;
  static char *argv[] = {"deathsmiles", NULL};
  threads_set_main();
  dslog("calling main() at game+%#x", GAMEADDR(main_addr));
  int r = ((int (*)(int, char **))main_addr)(1, argv);
  dslog("main() returned %d", r);
  return 0;
}

int main(void) {
  dslog_init();
  dslog("Deathsmiles Vita 1.0. Build " __DATE__ " " __TIME__);
  KuKernelExceptionHandlerOpt opt = {sizeof(opt)};
  for (int t = 0; t < 3; t++) kuKernelRegisterExceptionHandler(t, on_exception, NULL, &opt);

  // First launch: build game.bin from the iOS executable copied by the user (convert.c).
  { SceIoStat st; FILE *g = fopen("ux0:data/deathsmiles/game.bin", "rb"); char magic[4] = {0};
    if (g) { fread(magic, 1, 4, g); fclose(g); }
    if (memcmp(magic, "DSLD", 4)) {
      dslog("First launch: preparing the game files, please wait...");
      uint64_t t0 = sceKernelGetProcessTimeWide(); char err[256];
      int r = convert_game("ux0:data/deathsmiles/app/deathsmiles", "ux0:data/deathsmiles/game.tmp", err, sizeof err);
      if (r == 0) { sceIoRemove("ux0:data/deathsmiles/game.bin"); sceIoRename("ux0:data/deathsmiles/game.tmp", "ux0:data/deathsmiles/game.bin");
                    dslog("[OK] game.bin created in %.1f s", (sceKernelGetProcessTimeWide() - t0) / 1e6); }
      else { dslog("[error] %s", err); dslog("Press START to exit."); }
      (void)st;
    } }
  uint32_t main_addr = 0;
  if (load_game("ux0:data/deathsmiles/game.bin", &main_addr) == 0) {
    SceUID th = sceKernelCreateThread("ds_main", game_thread, 0x40, 2 * 1024 * 1024, 0, 0x10000 /* core 0 */, NULL);
    sceKernelStartThread(th, sizeof(main_addr), &main_addr);
    sceKernelWaitThreadEnd(th, NULL, NULL);
  }
  dslog("The game has stopped. Press START to exit.");
  for (;;) {
    SceCtrlData pad; sceCtrlPeekBufferPositive(0, &pad, 1);
    if (pad.buttons & SCE_CTRL_START) break;
    sceKernelDelayThread(50000);
  }
  { dslog_flush(); sceKernelExitProcess(0); }
  return 0;
}
