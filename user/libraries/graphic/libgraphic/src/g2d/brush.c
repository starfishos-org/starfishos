/*
 * Implementation of brush.
 */
#include "brush.h"

#include "../dep/os.h"


PBRUSH _create_brush()
{
	PBRUSH brush;
	brush = (PBRUSH)malloc(sizeof(*brush));
	return brush;
}

inline void _del_brush(PBRUSH brush)
{
	free(brush);
}


inline const RGBA *brush_color(const PBRUSH brush)
{
	return &brush->color;
}

inline u32 brush_width(const PBRUSH brush)
{
	return brush->width;
}


inline void set_brush_color(PBRUSH brush, RGBA color)
{
	brush->color = color;
}

inline void set_brush_width(PBRUSH brush, u32 width)
{
	brush->width = width;
}
