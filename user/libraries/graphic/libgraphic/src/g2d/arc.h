/*
 * Arc header file.
 */
#pragma once

#include "g2d_internal.h"


int _draw_arc(const PDC dc, int x1, int y1, int x2, int y2,
	int x3, int y3, int x4, int y4);
int _draw_ellipse(const PDC dc, int l, int t, int r, int b);
