#include <arch/drivers/multiboot2.h>
#include <common/types.h>

/* Multiboot2 infomation */
static struct multiboot_tag_mmap *mb2_tag_mmap = NULL;
static struct multiboot_tag_framebuffer *mb2_tag_fb = NULL;
static struct multiboot_tag_acpi *mb2_tag_acpi = NULL;

/* parse multiboot2 infomation */
void parse_mb2_info(u64 magic, vaddr_t addr)
{
	struct multiboot_tag *tag;

	/*  Check multiboot2 info magic */
	if (magic != MULTIBOOT2_BOOTLOADER_MAGIC)
		BUG("Invalid multiboot_info magic number: 0x%x\n", (u32)magic);

	/* Check alignment */
	if (addr & 7)
		BUG("Unaligned mbi: 0x%x\n", addr);

	for (tag = (struct multiboot_tag *)(addr + 8);
	     tag->type != MULTIBOOT_TAG_TYPE_END;
	     tag = (struct multiboot_tag *)((u64)tag + ((tag->size + 7) & ~7))) {
		switch (tag->type) {
#if 0
		case MULTIBOOT_TAG_TYPE_CMDLINE:
			kdebug("[Multiboot2 INFO] Command line = %s\n",
			       ((struct multiboot_tag_string *)tag)->str);
			break;
		case MULTIBOOT_TAG_TYPE_BOOT_LOADER_NAME:
			kdebug("[Multiboot2 INFO] Boot loader name = %s\n",
			       ((struct multiboot_tag_string *)tag)->str);
			break;
		case MULTIBOOT_TAG_TYPE_BASIC_MEMINFO:
			kdebug("[Multiboot2 INFO] mem_lower = %uKB, mem_upper = %uKB\n",
			       ((struct multiboot_tag_basic_meminfo *)tag)->mem_lower,
			       ((struct multiboot_tag_basic_meminfo *)tag)->mem_upper);
			break;
		case MULTIBOOT_TAG_TYPE_BOOTDEV:
			kdebug("[Multiboot2 INFO] Boot device 0x%x,%u,%u\n",
			       ((struct multiboot_tag_bootdev *)tag)->bios_dev,
			       ((struct multiboot_tag_bootdev *)tag)->slice,
			       ((struct multiboot_tag_bootdev *)tag)->part);
			break;
#endif
		case MULTIBOOT_TAG_TYPE_MMAP:
			mb2_tag_mmap = (struct multiboot_tag_mmap *)tag;
			break;
		case MULTIBOOT_TAG_TYPE_ACPI_OLD:
			mb2_tag_acpi = (struct multiboot_tag_acpi *)tag;
			kinfo("[Multiboot2 INFO] Found old acpi tag, size = %d, rsdp = 0x%lx\n",
			      mb2_tag_acpi->size,
			      mb2_tag_acpi->rsdp);
			break;
		case MULTIBOOT_TAG_TYPE_ACPI_NEW:
			mb2_tag_acpi = (struct multiboot_tag_acpi *)tag;
			kinfo("[Multiboot2 INFO] Found new acpi tag, size = %d, rsdp = 0x%lx\n",
			      mb2_tag_acpi->size,
			      mb2_tag_acpi->rsdp);
			break;
		case MULTIBOOT_TAG_TYPE_FRAMEBUFFER: {
			mb2_tag_fb = (struct multiboot_tag_framebuffer *)tag;

			switch (mb2_tag_fb->common.framebuffer_type) {
			case MULTIBOOT_FRAMEBUFFER_TYPE_INDEXED:
				BUG("Framebuffer type %d unsupported\n",
				    MULTIBOOT_FRAMEBUFFER_TYPE_INDEXED);

			case MULTIBOOT_FRAMEBUFFER_TYPE_RGB:
				kinfo("[Multiboot2 INFO] Detect framebuffer type RGB, "
				      "width = %d, height = %d, depth = %d, addr = 0x%lx\n",
				      mb2_tag_fb->common.framebuffer_width,
				      mb2_tag_fb->common.framebuffer_height,
				      mb2_tag_fb->common.framebuffer_bpp,
				      mb2_tag_fb->common.framebuffer_addr);
				break;

			case MULTIBOOT_FRAMEBUFFER_TYPE_EGA_TEXT:
				kinfo("[Multiboot2 INFO] Detect framebuffer type EGA TEXT, "
				      "width = %d, height = %d, depth = %d, addr = 0x%lx\n",
				      mb2_tag_fb->common.framebuffer_width,
				      mb2_tag_fb->common.framebuffer_height,
				      mb2_tag_fb->common.framebuffer_bpp,
				      mb2_tag_fb->common.framebuffer_addr);
				break;

			default:
				break;
			}
		} break;
		}
	}
	if (!mb2_tag_mmap)
		BUG("No multiboo2 mmap infomation\n");

	if (!mb2_tag_acpi)
		BUG("No multiboo2 ACPI infomation\n");

	// tag = (struct multiboot_tag *)((u64)tag + ((tag->size + 7) & ~7));
	// kinfo("[Multiboot2 INFO] Total multiboot2 info size 0x%x\n", (u64)tag - addr);
}

struct multiboot_tag_mmap *get_mb2_mmap(void)
{
	BUG_ON(!mb2_tag_mmap);
	return mb2_tag_mmap;
}

struct multiboot_tag_framebuffer *get_mb2_fb(void)
{
	BUG_ON(!mb2_tag_fb);
	return mb2_tag_fb;
}

struct multiboot_tag_acpi *get_mb2_acpi(void)
{
	BUG_ON(!mb2_tag_acpi);
	return mb2_tag_acpi;
}
