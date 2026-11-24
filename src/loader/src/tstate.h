#pragma once
#include <stdint.h>
#include <psp2/kernel/threadmgr.h>
// Per-thread state: autorelease stack and "context" GL state (each game thread has its own EAGLContext).
typedef struct {
  SceUID tid;
  void **pool; uint32_t pool_n, pool_cap;
  void *ctx;                 // EAGLContext actual
  uint32_t tex, fb, rb;      // 2D texture, framebuffer and renderbuffer bound on this thread
  int gl_seen;
  int vp[4], sc[4], vp_init;
  char last_file[16];        // last data*.bin read on this thread (to know which file each texture comes from)   // viewport/scissor as the game set them on this thread (its context)
} TState;
TState *ts_get(void);
void ts_release(void);
void threads_set_main(void);
uintptr_t threads_resolve_symbol(const char *name);
void threads_init(void);
