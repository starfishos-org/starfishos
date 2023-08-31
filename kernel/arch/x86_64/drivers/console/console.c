#include <arch/drivers/multiboot2.h>
#include <arch/mmu.h>

#include "fb.h"

void init_console(void)
{
	/* Init graphic output */
	if (get_mb2_fb()->common.framebuffer_type
		   == MULTIBOOT_FRAMEBUFFER_TYPE_RGB) {
		fb_console_init(get_mb2_fb()->common.framebuffer_width,
			       get_mb2_fb()->common.framebuffer_height,
			       get_mb2_fb()->common.framebuffer_bpp,
			       phys_to_virt(get_mb2_fb()->common.framebuffer_addr));
		set_graphic_putc_handler(fb_console_putc);
		kinfo("[ChCore] framebuffer console init finished\n");
	}
	else if (get_mb2_fb()->common.framebuffer_type
	    == MULTIBOOT_FRAMEBUFFER_TYPE_EGA_TEXT) {
		kinfo("[ChCore] VGA text mode not supported\n");
	}
	else {
		kinfo("[ChCore] no avaliable graphic output\n");
	}
}

