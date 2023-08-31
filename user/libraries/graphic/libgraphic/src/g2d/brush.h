/*
 * Brush header file.
 */
#pragma once

#include "g2d_internal.h"


struct brush
{
	RGBA color;
	u32 width;
};


PBRUSH _create_brush();
void _del_brush(PBRUSH brush);

const RGBA *brush_color(const PBRUSH brush);
u32 brush_width(const PBRUSH brush);

void set_brush_color(PBRUSH brush, RGBA color);
void set_brush_width(PBRUSH brush, u32 width);
