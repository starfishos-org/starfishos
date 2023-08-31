/*
   MIT License

   Copyright (c) 2017 Leon de Boer

   Permission is hereby granted, free of charge, to any person obtaining a copy
   of this software and associated documentation files (the "Software"), to deal
   in the Software without restriction, including without limitation the rights
   to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
   copies of the Software, and to permit persons to whom the Software is
   furnished to do so, subject to the following conditions:

   The above copyright notice and this permission notice shall be included in all
   copies or substantial portions of the Software.

   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
   IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
   FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
   AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
   LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
   OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
   SOFTWARE.
*/

/*
 * ChCore refers to
 * https://github.com/LdB-ECM/Raspberry-Pi/blob/master/Arm32_64_USB/rpi-smartstart.h
 * for the framebuffer operations.
 */

#include <common/lock.h>
#include <common/util.h>

#include "fb.h"
#include "font8x16.h"

/* Global framebuffer lock */
struct lock fb_lock;

console_t __attribute__((aligned(4))) console = {0};

#define LINE_MAX_CHAR_NUM console.wth / BITFONTWTH
#define MAX_LINE_NUM      console.ht / BITFONTHT

/* Move all lines up n pixels. */
static void move_lines_up(console_t *dc, int n_pixels)
{
	u64 video_wr_ptr_0 = dc->fb; // line 0 base addr
	u64 video_wr_ptr_n =
		dc->fb + (n_pixels * dc->pitch); // line n base addr

	u32 xs, ys, size;
	ys = console.ht - n_pixels;
	xs = console.wth;
	size = xs * ys * dc->depth / 8;

	memcpy((char *)video_wr_ptr_0, (char *)video_wr_ptr_n, size);
}

/* Clear the characters between (x1, y1) and (x2, y2). */
static void clear_area(console_t *dc, u32 x1, u32 y1, u32 x2, u32 y2)
{
	u64 video_wr_ptr;
	u32 y;

	video_wr_ptr = dc->fb + (y1 * dc->pitch) + (x1 * dc->depth / 8);

	for (y = 0; y < (y2 - y1); y++) {	// For each y line
		for (u32 x = 0; x < (x2 - x1);
		     x++) {			// For each x between x1 and x2
			switch (dc->depth) {
			case 16: {
				RGB565 bc;	// Colour for background
				bc.r = dc->bk_color.rgb_red >> 3;
				bc.g = dc->bk_color.rgb_green >> 2;
				bc.b = dc->bk_color.rgb_blue >> 3;
				RGB565 *video_wr_ptr_16 =
					(RGB565 *)video_wr_ptr;
				video_wr_ptr_16[x] = bc; // Write pixel
				break;
			}
			case 24: {
				RGB *video_wr_ptr_24 = (RGB *)video_wr_ptr;
				video_wr_ptr_24[x] =
					dc->bk_color.rgb; // Write pixel
				break;
			}
			case 32: {
				RGBA *video_wr_ptr_32 = (RGBA *)video_wr_ptr;
				video_wr_ptr_32[x] =
					dc->bk_color; // Write pixel
				break;
			}
			default:
				BUG("Unsupported depth\n");
			}
		}
		video_wr_ptr += dc->pitch; // Next line down
	}
}

static void write_char(console_t *dc, u8 ch)
{
	u64 video_wr_ptr, b;
	u32 i, y;
	u8 xoffs;

	video_wr_ptr = dc->fb + (dc->cur_pos.y * dc->pitch)
		       + (dc->cur_pos.x * dc->depth / 8);

	for (y = 0; y < 4; y++) {
		b = bit_font[(ch * 4) + y];	// Fetch character bits
		for (i = 0; i < 32; i++) {	// For each bit
			xoffs = i % 8;		// X offset
			switch (dc->depth) {
			case 16: {
				RGB565 fc, bc, col;
				fc.r = dc->txt_color.rgb_red >> 3;
				fc.g = dc->txt_color.rgb_green >> 2;
				fc.b = dc->txt_color.rgb_blue
				       >> 3;	// Colour for text
				bc.r = dc->bk_color.rgb_red >> 3;
				bc.g = dc->bk_color.rgb_green >> 2;
				bc.b = dc->bk_color.rgb_blue
				       >> 3;	// Colour for background
				col = bc;	// Preset background colour
				if ((b & 0x80000000) != 0)
					col = fc;	// If bit set take text colour
				RGB565 *video_wr_ptr_16 =
					(RGB565 *)video_wr_ptr;
				video_wr_ptr_16[xoffs] = col; // Write pixel
				break;
			}
			case 24: {
				RGB col = dc->bk_color.rgb;	// Preset background colour
				if ((b & 0x80000000) != 0)
					col = dc->txt_color.rgb; // If bit set take text colour
				RGB *video_wr_ptr_24 = (RGB *)video_wr_ptr;
				video_wr_ptr_24[xoffs] = col;	// Write pixel
				break;
			}
			case 32: {
				RGBA col =
					dc->bk_color;	// Preset background colour
				if ((b & 0x80000000) != 0)
					col = dc->txt_color; // If bit set take text colour
				RGBA *video_wr_ptr_32 = (RGBA *)video_wr_ptr;
				video_wr_ptr_32[xoffs] = col; // Write pixel
				break;
			}
			default:
				BUG("Unsupported depth\n");
			}
			b <<= 1; // Roll font bits left
			if (xoffs == 7)
				video_wr_ptr +=
					dc->pitch; // If was bit 7, next line down
		}
	}
	dc->cur_pos.x += BITFONTWTH; // Increment x position
}

static void console_boundary_check(console_t *dc)
{
	/* Vertical boundary check */
	if (dc->cursor.x >= LINE_MAX_CHAR_NUM) {
		dc->cursor.x %= LINE_MAX_CHAR_NUM;
		dc->cursor.y++;
	}
	/* Horizontal boundary check */
	if (dc->cursor.y >= MAX_LINE_NUM) {
		/* Moving up lines to display the incoming line */
		move_lines_up(dc, BITFONTHT);
		/* Clear the remaining last line */
		clear_area(dc, 0, dc->ht - BITFONTHT, dc->wth, dc->ht);
		dc->cursor.y--;
	}
}

/*
 * Writes the given character to the console and performs cursor movements as
 * required by what the character is.
 */
void fb_console_putc(char ch)
{
	lock(&fb_lock);

	switch (ch) {
	case '\r': {			// Carriage return character
		console.cursor.x = 0;	// Cursor back to line start
	} break;
	case '\t': {			// Tab character character
		console.cursor.x += 5;	// Cursor increment to by 5
		console.cursor.x -= (console.cursor.x % 4); // align it to 4
	} break;
	case '\n': {			// New line character
		console.cursor.x = 0;	// Cursor back to line start
		console.cursor.y++;	// Increment cursor down a line
	} break;
	case '\b': {
		console.cursor.x--;
		if (console.cursor.x < 0) {
			console.cursor.x = LINE_MAX_CHAR_NUM - 1;
			console.cursor.y--;
		}
		clear_area(&console,
				console.cursor.x * BITFONTWTH,
				console.cursor.y * BITFONTHT,
				(console.cursor.x + 1) * BITFONTWTH,
				(console.cursor.y + 1) * BITFONTHT);
	} break;
	default: {			  // All other characters
		console.cur_pos.x = console.cursor.x * BITFONTWTH;
		console.cur_pos.y = console.cursor.y * BITFONTHT;
		write_char(&console, ch); // Write the character to graphics screen
		console.cursor.x++;	  // Cursor.x forward one character
	} break;
	}
	console_boundary_check(&console);

	unlock(&fb_lock);
}

void fb_console_init(int width, int height, int depth, u64 fb)
{
	lock_init(&fb_lock);

	console.fb = fb;
	console.txt_color.ref = 0xFFFFFFFF;	// black
	console.bk_color.ref = 0x00000000;	// white
	console.wth = width;
	console.ht = height;
	console.depth = depth;
	console.pitch = width * depth / 8;
	console.fb_size = console.pitch * height;
}
