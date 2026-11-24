#pragma once
void gfx_init(void);
void gfx_set_screen(int w, int h);
void gfx_present(void);
uintptr_t gl_resolve_symbol(const char *name);
#include <psp2/kernel/processmgr.h>
