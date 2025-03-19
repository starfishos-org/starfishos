#pragma once

/* Intel Vol 1 section 13.1 */
/*
0	X87	x87 FPU/MMX support (must be 1)
1	SSE	XSAVE support for MXCSR and XMM registers
2	AVX	AVX enabled and XSAVE support for upper halves of YMM registers
3	BNDREG	MPX enabled and XSAVE support for BND0-BND3 registers
4	BNDCSR	MPX enabled and XSAVE support for BNDCFGU and BNDSTATUS registers
5	opmask	AVX-512 enabled and XSAVE support for opmask registers k0-k7
6	ZMM_Hi256	AVX-512 enabled and XSAVE support for upper halves of lower ZMM registers
7	Hi16_ZMM	AVX-512 enabled and XSAVE support for upper ZMM registers
9	PKRU	XSAVE support for PKRU register
*/
#define X86_XSAVE_STATE_X87 (1 << 0)
#define X86_XSAVE_STATE_SSE (1 << 1)
#define X86_XSAVE_STATE_AVX (1 << 2)

#define X86_XSAVE_STATE_OPMASK      (1 << 5)
#define X86_XSAVE_STATE_ZMM_Hi256   (1 << 6)
#define X86_XSAVE_STATE_Hi16_ZMM    (1 << 7)

#define XFEATURE_MASK_AVX512 (X86_XSAVE_STATE_OPMASK | X86_XSAVE_STATE_ZMM_Hi256 | X86_XSAVE_STATE_Hi16_ZMM)
