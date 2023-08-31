/*
 * Pixel header file.
 */
#pragma once

#include "g2d_internal.h"


int _draw_pixel(const PDC dc, int x, int y);
int _draw_pixels(const PDC dc, const POINT *points, int nr_points);
