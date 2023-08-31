/*
 * Implementation of arc functions.
 */
#include "arc.h"

#include "dc.h"


int _draw_arc(const PDC dc, int x1, int y1, int x2, int y2,
	int x3, int y3, int x4, int y4)
{
	dc_gfx(dc)->draw_arc(dc, x1, y1, x2, y2, x3, y3, x4, y4);
	return 0;
}

int _draw_ellipse(const PDC dc, int l, int t, int r, int b)
{
	dc_gfx(dc)->draw_ellipse(dc, l, t, r, b);
	return 0;
}
