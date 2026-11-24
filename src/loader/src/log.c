#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include "debugScreen.h"
#include "ds.h"
static SceUID g_logfd = -1;
static int g_lines;
// Each sceIoWrite to the memory card took ~12 ms and ran on the logging thread (usually the main thread):
// opening menus (dozens of trace lines) dropped frames. Lines now go to a RAM buffer and a low-priority
// thread writes them every 100 ms. Errors are written immediately.
#define LOGBUF (128 * 1024)
static char g_lb[LOGBUF]; static int g_lb_n;
static SceKernelLwMutexWork g_lmx; static int g_async;
static void flush_locked(void) { if (g_lb_n && g_logfd >= 0) sceIoWrite(g_logfd, g_lb, g_lb_n); g_lb_n = 0; }
static char g_wb[LOGBUF];
static int log_thread(SceSize a, void *p) {
  for (;;) {
    sceKernelDelayThread(100 * 1000);
    sceKernelLockLwMutex(&g_lmx, 1, NULL);
    int n = g_lb_n; if (n) { memcpy(g_wb, g_lb, n); g_lb_n = 0; }
    sceKernelUnlockLwMutex(&g_lmx, 1);
    if (n && g_logfd >= 0) sceIoWrite(g_logfd, g_wb, n);   // the slow write, outside the lock
  }
  return 0;
}
void dslog_init(void) {
  sceIoMkdir("ux0:data/deathsmiles", 0777);
  g_logfd = sceIoOpen("ux0:data/deathsmiles/log.txt", SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
  psvDebugScreenInit();
  int r1 = sceKernelCreateLwMutex(&g_lmx, "log", 0, 0, NULL), r2 = -1, r3 = -1;
  if (r1 >= 0) {
    r2 = sceKernelCreateThread("ds_log", log_thread, 160, 0x4000, 0, 0, NULL);   // prioridad absoluta baja (64 alta .. 191 baja)
    if (r2 >= 0) { r3 = sceKernelStartThread(r2, 0, NULL); if (r3 >= 0) g_async = 1; }
  }
  dslog("log: %s writes (mutex %x, thread %x, start %x)", g_async ? "in the background" : "DIRECTA (lenta)", r1, r2, r3);
}
void dslog(const char *fmt, ...) {
  char buf[512];
  unsigned ms = (unsigned)(sceKernelGetProcessTimeWide() / 1000);
  int n = snprintf(buf, sizeof(buf), "[%6u.%03u] ", ms / 1000, ms % 1000);
  va_list ap; va_start(ap, fmt); n += vsnprintf(buf + n, sizeof(buf) - n - 2, fmt, ap); va_end(ap);
  if (n > (int)sizeof(buf) - 2) n = sizeof(buf) - 2;
  buf[n++] = '\n'; buf[n] = 0;
  if (g_logfd >= 0) {
    if (!g_async) sceIoWrite(g_logfd, buf, n);
    else {
      int urgent = strstr(buf, "[error") || strstr(buf, "fatal") || strstr(buf, "excepci") || strstr(buf, "[crash");
      sceKernelLockLwMutex(&g_lmx, 1, NULL);
      if (g_lb_n + n > LOGBUF) flush_locked();          // buffer full (should not happen): write now
      memcpy(g_lb + g_lb_n, buf, n); g_lb_n += n;
      if (urgent) flush_locked();                        // errors: to disk right away (in case the game hangs afterwards)
      sceKernelUnlockLwMutex(&g_lmx, 1);
    }
  }
  extern int g_gfx_active;
  if (g_gfx_active) return;            // with vitaGL active the text screen is not visible: do not spend CPU on it
  if (++g_lines > 30) { psvDebugScreenPuts("\e[2J\e[H"); g_lines = 0; }
  psvDebugScreenPrintf("%s", buf + 13);
}

void dslog_flush(void) {
  if (!g_async) return;
  sceKernelLockLwMutex(&g_lmx, 1, NULL); flush_locked(); sceKernelUnlockLwMutex(&g_lmx, 1);
}
