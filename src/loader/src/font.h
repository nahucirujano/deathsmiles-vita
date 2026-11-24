#pragma once
#include <stdint.h>
void font_init(void);
float font_line_height(float size);
void font_measure(const char *t, float size, float maxw, int maxlines, float *w, float *h);
// rgba = 0xRRGGBBAA
void font_draw(uint8_t *buf, int bw, int bh, int pitch, const char *t, float x, float y, float boxw, float size,
               uint32_t rgba, int align, float maxw, int maxlines);
