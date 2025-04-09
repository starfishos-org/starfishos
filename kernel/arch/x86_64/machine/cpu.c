#include <common/types.h>
#include <common/kprint.h>
#include <common/macro.h>
#include <arch/config.h>
#include <machine.h>
#include <sched/fpu.h>

#include "xsave.h"

#define CR0_TS 3 /* task switched: related to FPU instructions */
#define CR0_PE                                                           \
    0 /* protection enable. To enable paging, both PE and PG must be set \
       */
#define CR0_ET 4 /* Extension Type. Hardcoded to 1. */
#define CR0_WP                                                        \
    16 /* write protection. When set, inhibits kernel from writing RO \
          pages */
#define CR0_AM 18 /* alignment check */
#define CR0_NW 29 /* not write-through */
#define CR0_CD 30 /* cache disable */
#define CR0_PG 31 /* paging */

#define CR0_NE 5 /* enable x87 execption */

#if 0
/* cr3 contains pgtbl paddr and two flags (PCD and PWT) */
/* two-flag controls page-structure caching */
#define CR3_PCD 4 /* page-level cache disable */
#define CR3_PWT 3 /* page-level write-through */
#endif

#if 1
#define CR4_PVI        1 /* protected-mode virtual interrupts */
#define CR4_PCE        8 /* performance monitoring counter enable */
#define CR4_OSFXSR     9 /* OS support for FXSAVE and FXRSTOR instructions */
#define CR4_OSXMMEXCPT 10 /* OS support for unmasked SIMD exceptions */
#define CR4_SMXE       14 /* Safer Mode Extensions enable bit */
#define CR4_OSXSAVE    18 /* FPU and xcr0 related */
#endif

#define CR4_TSD 2 /* time stamp disable */
#define CR4_PSE                                                  \
    4 /* page size extensions; enable 4M-page with 32-bit paging \
       */
#define CR4_PAE \
    5 /* physical address extension. must be set before entering IA-32e*/
/*
 * PGE:
 * The global page feature allows frequently used or shared pages to be marked
 * as global to all users (global flag).
 *
 * Global pages are not flushed on a task switch or a write to register CR3.
 *
 */
#define CR4_PGE 7 /* page global enable */
/* UMIP: only present in new Intel processosrs */
#define CR4_UMIP 11 /* user-mode instruction prevention. */
/* disables SGDT, SIDT, SLDT, SMSW, and STR if CPL > 0 */
#define CR4_VMXE     13 /* enable VMX operation when set */
#define CR4_FSGSBASE 16 /* enable: RDFSBASE, RDGSBASE, WRFSBASE, WRGSBASE */
/*
 * PCID:
 * A PCID is a 12-bit identifer.
 * The current PCID is the value of bits 11:0 of CR3.
 * Software can change CR4.PCIDE from 0 to 1 only if CR3[11:0] = 0
 *
 * When a logical processor creates entries in the TLBs and paging-structure
 * caches, it associates those entries with the current PCID. When using entries
 * in the TLBs and paging-structure caches to translate a linear address, a
 * logical processor uses only those entries associated with the current PCID.
 *
 * One exception: a logical processor may use a global TLB entry to translate a
 * linear address, even if the TLB entry is associated with a PCID different
 * from the current PCID.
 *
 */
#define CR4_PCIDE 17 /* enables process-context identifiers */
#define CR4_SMEP  20 /* SMEP */
#define CR4_SMAP  21 /* SMAP */
#define CR4_PKE   22 /* PKRU enable bit */

#if 0
#define CR8_TPL /* interrupt block related */
#endif

static u64 get_cr0(void)
{
    u64 cr0;

    asm volatile("mov %%cr0, %0\n\t" : "=r"(cr0)::);
    return cr0;
}

static void set_cr0(u64 cr0)
{
    asm volatile("mov %0, %%cr0\n\t" : : "r"(cr0) :);
}

static u64 get_cr3(void)
{
    u64 cr3;

    asm volatile("mov %%cr3, %0\n\t" : "=r"(cr3)::);
    return cr3;
}

static void set_cr3(u64 cr3)
{
    asm volatile("mov %0, %%cr3\n\t" : : "r"(cr3) :);
}

static u64 get_cr4(void)
{
    u64 cr4;

    asm volatile("mov %%cr4, %0\n\t" : "=r"(cr4)::);
    return cr4;
}

static void set_cr4(u64 cr4)
{
    asm volatile("mov %0, %%cr4\n\t" : : "r"(cr4) :);
}

static u64 test_bit(u64 value, u64 bit)
{
    return value | (1 << bit);
}

static u64 _set_bit(u64 value, u64 bit)
{
    return value | (1 << bit);
}

static u64 _clear_bit(u64 value, u64 bit)
{
    return value & (~(1 << bit));
}

#if MPK_ENABLED == 1
int rdpkru(void)
{
    int pkru;

    asm volatile("mov $0, %%rcx\n\t"
                 "rdpkru\n\t"
                 : "=a"(pkru)
                 :
                 : "rcx", "rdx", "memory");
    return pkru;
}

void wrpkru(int pkru)
{
    asm volatile("xor %%rcx, %%rcx\n\t"
                 "xor %%rdx, %%rdx\n\t"
                 "wrpkru\n\t"
                 :
                 : "a"(pkru)
                 : "rcx", "rdx", "memory");
}
#endif

/* enable/disable turbo boost */
#define TURBO_DISABLE_BIT  38
#define TURBO_DISABLE_MASK (1 << (38 - 32))
#define TURBO_ENABLE_MASK  (~TURBO_DISABLE_MASK)
void set_turbo_boost(void)
{
    u32 hi, lo;

    hi = 0;
    lo = 0;

    asm volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(0x1a0));
    kdebug("%s, rdmsr: hi=%08x lo=%08x\n", __func__, hi, lo);
#if TURBO_ENABALED == 1
    /* enable CPU turbo boost */
    hi = hi & TURBO_ENABLE_MASK;
    kdebug("Intel Turbo Boost is ENABLED.\n");
#else
    /* disable CPU turbo boost */

    hi = hi | TURBO_DISABLE_MASK;
    kdebug("Intel Turbo Boost is DISABLED.\n");
#endif
    asm volatile("wrmsr" ::"c"(0x1a0), "a"(lo), "d"(hi));
}

void enable_lbr(void);

void x86_xsetbv(u32 reg, u64 val)
{
    asm volatile("xsetbv"
                 :
                 : "c"(reg), "d"((u32)(val >> 32)), "a"((u32)val)
                 : "memory");
}

void ldmxcsr(u32 mxcsr)
{
    asm volatile("ldmxcsr %0" ::"m"(mxcsr));
}

static inline void __cpuid_count(u32 leaf, u32 subleaf, u32 *eax, u32 *ebx, u32 *ecx, u32 *edx)
{
    asm volatile("cpuid"
                : "=a"(*eax), "=b"(*ebx), "=c"(*ecx), "=d"(*edx)
                : "a"(leaf), "c"(subleaf)
                : "memory");
}

u32 xsave_area_size;
void arch_get_cpu_xsave_area_size(void)
{
    u32 eax, ebx, ecx, edx;
    
    __cpuid_count(0xD, 0, &eax, &ebx, &ecx, &edx);
    
    xsave_area_size = ROUND_UP(ebx, 0x1000);
    
    kdebug("XSAVE area size: %d bytes\n", xsave_area_size);
}

/* SMP: set the control registers on each core */
void arch_cpu_init(void)
{
    u64 cr0, cr4;

    cr0 = get_cr0();
    cr4 = get_cr4();

    kdebug("[ChCore] initial: cr0 is 0x%lx, cr4 is 0x%lx\n", cr0, cr4);

    if ((test_bit(cr0, CR0_PE) == 0) || (test_bit(cr0, CR0_PG) == 0))
        BUG("Bad initial CR0 value\n");

    /* enable write-protection in kernel */
    cr0 = _set_bit(cr0, CR0_WP);

    /* enable fpu exception */
    cr0 = _set_bit(cr0, CR0_NE);

    /* enable caching */
    cr0 = _clear_bit(cr0, CR0_NW);
    cr0 = _clear_bit(cr0, CR0_CD);

    set_cr0(cr0);

    if (test_bit(cr4, CR4_PSE) == 0)
        BUG("Bad initial CR4 value\n");

    /* The following two lines disable SMEP and SMAP */
    // cr4 = _clear_bit(cr4, CR4_SMEP);
    // cr4 = _clear_bit(cr4, CR4_SMAP);

    /* disable VMXE */
    cr4 = _clear_bit(cr4, CR4_VMXE);

    /* enable RDTSC */
    cr4 = _clear_bit(cr4, CR4_TSD);
    /* enable global page */
    cr4 = _set_bit(cr4, CR4_PGE);
    /* enable W/R FS/GS */
    cr4 = _set_bit(cr4, CR4_FSGSBASE);
    /* enable PCID */
    cr4 = _set_bit(cr4, CR4_PCIDE);

    /* enable FPU */
    cr4 = _set_bit(cr4, CR4_OSXSAVE);
    cr4 = _set_bit(cr4, CR4_OSFXSR);
    cr4 = _set_bit(cr4, CR4_OSXMMEXCPT);

    // x86_xsetbv(0, X86_XSAVE_STATE_X87);

#if MPK_ENABLED == 1
    /* enable PKEY on supported machines */
    cr4 = _set_bit(cr4, CR4_PKE);
    set_cr4(cr4);
    kinfo("PKRU at boot time : %x\n", rdpkru());
    wrpkru(0);
#endif

    set_cr4(cr4);

    x86_xsetbv(0,
               X86_XSAVE_STATE_X87 | X86_XSAVE_STATE_SSE | X86_XSAVE_STATE_AVX);
    // get CPU xsave area size
    arch_get_cpu_xsave_area_size();

    cr0 = get_cr0();
    cr4 = get_cr4();
    kdebug("[ChCore] configure: cr0 is 0x%lx, cr4 is 0x%lx\n", cr0, cr4);

    asm volatile("finit" ::: "memory");
    ldmxcsr(0x1f80);

    set_turbo_boost();

#if LBR_ENABLED == 1
    enable_lbr();
#endif
}

#define MSR_DEBUGCTLB  0x1D9
#define LBR_BIT        0
#define MSR_LBR_SELECT 0x1C8

void enable_lbr(void)
{
    u64 val;

    val = rdmsr(MSR_DEBUGCTLB);
    // kinfo("DEBUGCTLB: 0x%lx\n", val);

    wrmsr(MSR_DEBUGCTLB, val | (1 << LBR_BIT));

    /* With branch filter, the overhead can be even lower */
    /* not supported in qemu (till version 4.1) */
    // wrmsr(MSR_LBR_SELECT, 0);
}

#if FPU_SAVING_MODE == LAZY_FPU_MODE

void enable_fpu_usage(void)
{
    asm volatile("clts" ::: "memory");

    /* Set fpu_disable in the per_cpu info to ENABLE (0) */
    asm volatile("movl %0, %%gs:%c1"
                 :
                 : "r"(0), "i"(OFFSET_FPU_DISABLE)
                 : "memory");
}

void disable_fpu_usage(void)
{
    u32 fpu_disable = 0;

    /* Get fpu_disable in the per_cpu info */
    asm volatile("movl %%gs:%c1, %0"
                 : "=r"(fpu_disable)
                 : "i"(OFFSET_FPU_DISABLE)
                 : "memory");

    if (fpu_disable == 0) {
        /* Since fpu is not disabled, disable it now */
        set_cr0(_set_bit(get_cr0(), CR0_TS));

        fpu_disable = 1;

        /* Set fpu_disable in the per_cpu info to DISABLE (1) */
        asm volatile("movl %0, %%gs:%c1"
                     :
                     : "r"(fpu_disable), "i"(OFFSET_FPU_DISABLE)
                     : "memory");
    }
}

#endif

void flush_boot_tlb(void)
{
    u64 cr4;

    cr4 = get_cr4();
    if (test_bit(cr4, CR4_PGE)) {
        /* Clear CR4_PGE will flush all TLBs */
        cr4 = _clear_bit(cr4, CR4_PGE);
        set_cr4(cr4);
        /* Restore old CR4 */
        cr4 = _set_bit(cr4, CR4_PGE);
        set_cr4(cr4);
    } else {
        /* Write CR3 for flush all TLBs */
        set_cr3(get_cr3());
    }
}
