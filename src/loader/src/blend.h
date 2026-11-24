// "Over" blending of non-premultiplied RGBA8 without divisions (the Vita's Cortex-A9 has no hardware divide:
// each '/' is a ~40-80 cycle call; there used to be ~10 per pixel in UIKit text and images).
#pragma once
#include <stdint.h>
static inline int mul255(int x) { x += 128; return (x + (x >> 8)) >> 8; }   // x / 255 redondeado (x ≤ 255*255)
extern uint32_t g_recip[256];                                            // 65536*255/n (n = output alpha)
void blend_init(void);
static inline void blend_px(uint8_t *d, int r, int g, int b, int a) {
  if (a <= 0) return;
  int da = d[3];
  if (a >= 255 || da == 0) { d[0] = r; d[1] = g; d[2] = b; d[3] = a > 255 ? 255 : a; return; }
  int ia = 255 - a;
  if (da == 255) {   // opaque destination (the most common case): no division
    d[0] = mul255(r * a + d[0] * ia); d[1] = mul255(g * a + d[1] * ia); d[2] = mul255(b * a + d[2] * ia); return;
  }
  int db = mul255(da * ia), oa = a + db;           // destination contribution and resulting alpha
  uint32_t rc = g_recip[oa];                       // ≈ 65536/oa * 255 / 255
  d[0] = (uint32_t)(r * a + d[0] * db) * rc >> 24;
  d[1] = (uint32_t)(g * a + d[1] * db) * rc >> 24;
  d[2] = (uint32_t)(b * a + d[2] * db) * rc >> 24;
  d[3] = oa;
}
