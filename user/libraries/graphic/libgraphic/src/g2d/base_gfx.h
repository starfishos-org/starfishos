/*
 * Graphics function header file.
 */
#pragma once

#include "g2d_internal.h"


enum BLT_OP {
	BLT_RASTER,
	BLT_BLEND,
	BLT_BLEND_RGBA,
	BLT_TRANSPARENT
};

/* Base graphic functions */
struct base_gfx
{
	void (*draw_pixel)(const PDC dc, int x, int y);
	void (*draw_hline)(const PDC dc, int l, int r, int y);
	void (*draw_vline)(const PDC dc, int x, int t, int b);
	void (*draw_line)(const PDC dc, int x1, int y1, int x2, int y2);
	void (*fill_rect)(const PDC dc, int l, int t, int r, int b);
	void (*draw_arc)(const PDC dc, int x1, int y1, int x2, int y2,
		int x3, int y3, int x4, int y4);
	void (*draw_ellipse)(const PDC dc, int l, int t, int r, int b);
	void (*draw_char)(const PDC dc, char ch, int x, int y,
		int l, int t, int r, int b);
	void (*bitblt)(const PDC dst_dc, int x1, int y1,
		const PDC src_dc, int x2, int y2,
		int w, int h, enum BLT_OP bop, void *arg);
	void (*stretch_blt)(const PDC dst_dc, int x1, int y1, int sx1, int sy1, 
		const PDC src_dc, int x2, int y2, int sx2, int sy2,
		int px, int py, int pw, int ph, enum BLT_OP bop, void *arg);
};


struct base_gfx *get_base_gfx_16();
struct base_gfx *get_base_gfx_24();
struct base_gfx *get_base_gfx_32();
