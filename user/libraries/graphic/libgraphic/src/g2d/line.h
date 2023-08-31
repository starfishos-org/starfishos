/*
 * Line header file.
 */
#pragma once

#include "g2d_internal.h"


int _draw_hline(const PDC dc, int l, int r, int y);
int _draw_vline(const PDC dc, int x, int t, int b);
int _draw_line(const PDC dc, int x1, int y1, int x2, int y2);
int _draw_rect(const PDC dc, int l, int t, int r, int b);
int _fill_rect(const PDC dc, int l, int t, int r, int b);
