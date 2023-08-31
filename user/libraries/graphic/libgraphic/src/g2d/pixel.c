/*
 * Implementation of pixel functions.
 */
#include "pixel.h"

#include "dc.h"


int _draw_pixel(const PDC dc, int x, int y)
{
	if (!pt_in_dc(dc, x, y))
		return -1;
	dc_gfx(dc)->draw_pixel(dc, x, y);
	return 0;
}

int _draw_pixels(const PDC dc, const POINT *points, int nr_points)
{
	int i;
	for (i = 0; i < nr_points; ++i) {
		if (!pt_in_dc(dc, points[i].x, points[i].y))
			continue;
		dc_gfx(dc)->draw_pixel(dc, points[i].x, points[i].y);
	}
	return 0;
}
