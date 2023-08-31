/*
 * Blend header file.
 */
#pragma once

#include "g2d_internal.h"


void blend_pixel_16(RGB565 *dst, const RGB565 *src, u8 alpha);
void blend_pixel_24(RGB *dst, const RGB *src, u8 alpha);
void blend_pixel_32(RGBA *dst, const RGBA *src, u8 alpha);
void blend_pixel_rgba(RGBA *dst, const RGBA *src, u8 alpha);

int _blend(PDC dst, int x1, int y1, int w1, int h1,
	PDC src, int x2, int y2, int w2, int h2, u8 alpha);
int _blend_rgba(PDC dst, int x1, int y1, int w1, int h1,
	PDC src, int x2, int y2, int w2, int h2, u8 alpha);
