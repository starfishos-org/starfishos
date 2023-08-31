#include <seg.h>

struct segdesc __attribute__((aligned(16))) bootgdt[GDT_ENTRIES] = {
	[GDT_NULL] = SEGDESC(0,	/* base */
			     0,	/* base */
			     0	/* bits */
	    ),
	[GDT_CS_32] = SEGDESC(0,	/* base */
			      0xfffff,	/* limit */
			      SEG_R | SEG_CODE | SEG_S | SEG_DPL(0) | SEG_P | SEG_D | SEG_G	/* bits */
	    ),
	[GDT_CS_64] = SEGDESC(0,	/* base */
			      0,	/* limit */
			      SEG_R | SEG_CODE | SEG_S | SEG_DPL(0) | SEG_P | SEG_L | SEG_G	/* bits */
	    ),
	[GDT_DS] = SEGDESC(0,		/* base */
			   0xfffff,	/* limit */
			   SEG_W | SEG_S | SEG_DPL(0) | SEG_P | SEG_D | SEG_G	/* bits */
	    ),
	[GDT_FS] = SEGDESC(0,		/* base */
			   0xfffff,	/* limit */
			   SEG_W | SEG_S | SEG_DPL(0) | SEG_P | SEG_D | SEG_G	/* bits */
	    ),
	[GDT_GS] = SEGDESC(0,		/* base */
			   0xfffff,	/* limit */
			   SEG_W | SEG_S | SEG_DPL(0) | SEG_P | SEG_D | SEG_G	/* bits */
	    ),
	[GDT_UD] = SEGDESC(0,		/* base */
			   0xfffff,	/* limit */
			   SEG_W | SEG_S | SEG_DPL(3) | SEG_P | SEG_D | SEG_G	/* bits */
	    ),
	[GDT_UC] = SEGDESC(0,		/* base */
			   0,		/* limit */
			   SEG_R | SEG_CODE | SEG_S | SEG_DPL(3) | SEG_P | SEG_L | SEG_G	/* bits */
	    ),

	/* set GDT_TSS in kernel main */
};
