/*
 * Implementation of point functions.
 */
#include "g2d_internal.h"

#include "../dep/os.h"


POINT *create_point(int x, int y)
{
	POINT *pt = (POINT *)malloc(sizeof(*pt));
	pt->x = x;
	pt->y = y;
	return pt;
}

void delete_point(POINT *point)
{
	if (point != NULL)
		free(point);
}

inline void point_copy(POINT *dst, const POINT *src)
{
	dst->x = src->x;
	dst->x = src->x;
}
