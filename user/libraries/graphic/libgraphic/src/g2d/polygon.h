/*
 * Polygon header file.
 */
#pragma once

#include "g2d_internal.h"


int _draw_polygon(const PDC dc, const POINT *points, int nr_points);
int _draw_polyline(const PDC dc, const POINT *points, int nr_points);
