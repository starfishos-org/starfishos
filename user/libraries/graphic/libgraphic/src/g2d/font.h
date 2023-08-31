/*
 * Font header file.
 */
#pragma once

#include "g2d_internal.h"


PFONT _create_font(const char *fontbits_src, int byte_w, int byte_h,
	int byte_reverse);
void _del_font(PFONT font);
int _draw_char(const PDC dc, char ch, int x, int y);
PFONT _get_default_bitfont();

const char *font_bits(const PFONT font);
int font_byte_width(const PFONT font);
int font_byte_height(const PFONT font);
int font_byte_size(const PFONT font);
int font_byte_reverse(const PFONT font);
