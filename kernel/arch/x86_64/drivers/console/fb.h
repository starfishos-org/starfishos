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
 * for the data structures.
 */

#include <common/types.h>

#define BITFONTHT 16
#define BITFONTWTH 8

typedef struct
{
	int x;		// x co-ordinate
	int y;		// y co-ordinate
} POINT, *LPPOINT;	// Typedef define POINT and LPPOINT


typedef struct __attribute__((__packed__, aligned(1)))
{
	u32 rgb_blue : 8;	// Blue
	u32 rgb_green : 8;	// Green
	u32 rgb_red : 8;	// Red
} RGB;

typedef union
{
	struct __attribute__((__packed__, aligned(1)))
	{
		u32 rgb_blue : 8;		// Blue
		u32 rgb_green : 8;		// Green
		u32 rgb_red : 8;		// Red
		u32 rgb_alpha : 8;	        // Alpha
	};
	__attribute__((aligned(1))) RGB rgb;	// RGB triple (1st 3 bytes)
	u32 ref;				// Color reference
} RGBA;

typedef union
{
	struct __attribute__((__packed__, aligned(1)))
	{
		u32 b : 5;	// Blue
		u32 g : 6;	// Green
		u32 r : 5;	// Red
	};
	u16 ref;
} RGB565;

typedef union
{
	u8* raw_image;				        // Pointer to raw byte format array
	RGB565* __attribute__((aligned(2))) ptr_rgb565;	// Pointer to RGB565 format array
	RGB* __attribute__((aligned(1))) ptr_rgb;	// Pointer to RGB format array
	RGBA* __attribute__((aligned(4))) ptr_rgba;	// Pointer to RGBA format array
	u64 raw_ptr;				        // Pointer address
} HBITMAP;

/*--------------------------------------------------------------------------}
{						  INTERNAL DC  STRUCTURE							}
{--------------------------------------------------------------------------*/
typedef struct __attribute__((__packed__, aligned(4))) console {
	u64 fb;			        // Frame buffer address
	u32 fb_size;			// the whole framebuffer size
	u32 wth;			// Screen width (of frame buffer)
	u32 ht;				// Screen height (of frame buffer)
	u32 depth;			// Colour depth (of frame buffer)
	u32 pitch;			// Pitch (Line to line offset)

	/* Position control */
	POINT cur_pos;			// Current position of graphics pointer
	POINT cursor;			// Current cursor position

	/* Text colour control */
	RGBA txt_color;			// Text colour to write
	RGBA bk_color;			// Background colour to write
} console_t;

/* Exposed functions */
void fb_console_init(int width, int height, int depth, u64 fb);
void fb_console_putc(char ch);
