/*
 * Implementation of polygon functions.
 */
#include "polygon.h"

#include "line.h"


int _draw_polygon(const PDC dc, const POINT *points, int nr_points)
{
	int i;

	if (nr_points < 2)
		return -1;
	for (i = 0; i < nr_points - 1; ++i) {
		_draw_line(dc, points[i].x, points[i].y, points[i + 1].x, points[i + 1].y);
	}
	_draw_line(dc, points[i].x, points[i].y, points[i + 1].x, points[i + 1].y);
	return 0;
}

int _draw_polyline(const PDC dc, const POINT *points, int nr_points)
{
	int i;

	if (nr_points < 2)
		return -1;
	for (i = 0; i < nr_points - 1; ++i) {
		_draw_line(dc, points[i].x, points[i].y, points[i + 1].x, points[i + 1].y);
	}
	return 0;
}
