#pragma once
#include <stdint.h>
extern uintptr_t g_base; extern uint32_t g_vmbase, g_vmsize;
void dslog(const char *fmt, ...);
void dslog_init(void);
void dslog_flush(void);
// Release builds keep only important log lines. Developer diagnostics (per-texture/per-view traces, timing
// measurements, the SELECT+TRIANGLE draw dump) use dslogv and are compiled out unless DS_VERBOSE is 1.
#ifndef DS_VERBOSE
#define DS_VERBOSE 0
#endif
#define dslogv(...) do { if (DS_VERBOSE) dslog(__VA_ARGS__); } while (0)
uintptr_t resolve_import(const char *name, int is_data);
void *dummy_data(const char *name);
int load_game(const char *path, uint32_t *main_addr);
#define GAMEADDR(p) ((unsigned)((uintptr_t)(p) - g_base + g_vmbase))
#define SOFTFP __attribute__((pcs("aapcs")))
typedef struct { char name[16]; uint32_t addr, size; } GameSection;
extern GameSection g_sections[16]; extern uint32_t g_nsections;
const GameSection *game_section(const char *name);
void objc_bridge_init(void);
void objc_load_image(void);
uintptr_t objc_resolve_symbol(const char *name);
// realign_call copies up to 96 bytes above sp: threads entering the game leave stack headroom
#define STACK_PAD() volatile char _stack_pad[256]; _stack_pad[0] = 0; _stack_pad[255] = _stack_pad[0]
