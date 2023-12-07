// reference: linux-6.18/drivers/cxl/acpi.c
#include "acpi.h"
#include <common/util.h>
#include <common/macro.h>
#include <common/types.h>
#include <common/size.h>
#include <common/bitfield.h>
#include <arch/io.h>
#include <arch/drivers/cxl.h>
#include <drivers/cxl.h>

// #include "cedt.h"
#include "actbl1.h"

extern struct cxl_chbs_context cxl_chbs_ctxs[];
extern int cxl_chbs_ctxs_num;

extern struct cxl_mem_dev cxl_mem_devs[];
extern int cxl_mem_dev_num;

/* Encode defined in CXL 2.0 8.2.5.12.7 HDM Decoder Control Register */
static inline int eig_to_granularity(u16 eig, unsigned int *granularity)
{
        if (eig > CXL_DECODER_MAX_ENCODED_IG)
                return -1;
        *granularity = CXL_DECODER_MIN_GRANULARITY << eig;
        return 0;
}

/* Encode defined in CXL ECN "3, 6, 12 and 16-way memory Interleaving" */
static inline int eiw_to_ways(u8 eiw, unsigned int *ways)
{
        switch (eiw) {
        case 0 ... 4:
                *ways = 1 << eiw;
                break;
        case 8 ... 10:
                *ways = 3 << (eiw - 8);
                break;
        default:
                return -1;
        }

        return 0;
}

static int cxl_acpi_cfmws_verify(struct acpi_cedt_cfmws *cfmws)
{
        if (cfmws->interleave_arithmetic != ACPI_CEDT_CFMWS_ARITHMETIC_MODULO) {
                kdebug("CFMWS Unsupported Interleave Arithmetic\n");
                return -1;
        }

        if (!IS_ALIGNED(cfmws->base_hpa, SIZE_256M)) {
                kdebug("CFMWS Base HPA not 256MB aligned\n");
                return -1;
        }

        if (!IS_ALIGNED(cfmws->window_size, SIZE_256M)) {
                kdebug(dev, "CFMWS Window Size not 256MB aligned\n");
                return -1;
        }

        // TODO: add more verify entries

        return 0;
}
#if 0
static int __assign_cxlmemdev_to_chbs(struct cxl_mem_dev *dev)
{
        int idx = 0;
        for (idx = 0; idx < cxl_chbs_ctxs_num; idx++) {
                if (dev->)
        }
}
#endif

static inline int cxl_hdm_decoder_count(u32 cap_hdr)
{
        int val = FIELD_GET(CXL_HDM_DECODER_COUNT_MASK, cap_hdr);

        return val ? val * 2 : 1;
}

/*
 * hdm: CXL HDM Decoder Capability Structure
 */
static void enable_hdm_decoder(void *hdm)
{
        u32 global_ctrl;

        global_ctrl = readl(hdm + CXL_HDM_DECODER_CTRL_OFFSET);
        writel(global_ctrl | CXL_HDM_DECODER_ENABLE,
               hdm + CXL_HDM_DECODER_CTRL_OFFSET);
}

static void parse_hdm_decoder_caps(void *base, struct cxl_hdm *hdm)
{
        u32 hdm_cap;
        // u32 decoder_count, target_count, interleave_mask = 0;
        hdm->hdm_base = base;
        hdm_cap = readl(base + CXL_HDM_DECODER_CAP_OFFSET);
        hdm->decoder_count = cxl_hdm_decoder_count(hdm_cap);
        hdm->target_count =
                FIELD_GET(CXL_HDM_DECODER_TARGET_COUNT_MASK, hdm_cap);
        if (FIELD_GET(CXL_HDM_DECODER_INTERLEAVE_11_8, hdm_cap))
                hdm->interleave_mask |= GENMASK(11, 8);
        if (FIELD_GET(CXL_HDM_DECODER_INTERLEAVE_14_12, hdm_cap))
                hdm->interleave_mask |= GENMASK(14, 12);
        kinfo("[CXL] base: 0x%llx, decoder cnt: %d, target_count: %d, interleave_mask: %d\n",
              hdm->hdm_base,
              hdm->decoder_count,
              hdm->target_count,
              hdm->interleave_mask);
}

/*
 * Per CXL 2.0 8.2.5.12.20 Committing Decoder Programming, hardware must set
 * committed or error within 10ms, but just be generous with 20ms to account for
 * clock skew and other marginal behavior
 */
#define COMMIT_TIMEOUT_MS 20
static int cxld_await_commit(void *hdm, int id)
{
        u32 ctrl;
        int i;

        // while (true) {
        for (i = 0; i < COMMIT_TIMEOUT_MS; i++) {
                ctrl = readl(hdm + CXL_HDM_DECODER0_CTRL_OFFSET(id));
                if (FIELD_GET(CXL_HDM_DECODER0_CTRL_COMMIT_ERROR, ctrl)) {
                        ctrl &= CXL_HDM_DECODER0_CTRL_COMMIT;
                        writel(ctrl, hdm + CXL_HDM_DECODER0_CTRL_OFFSET(id));
                        return -100;
                }
                if (FIELD_GET(CXL_HDM_DECODER0_CTRL_COMMITTED, ctrl))
                        return 0;

                for (int loop = 0; loop < 1000000000; loop++)
                        ;
        }
        return -1;
}
#if 0
static void cxld_set_interleave(u32 *ctrl)
{
        u16 eig = 0;
        u8 eiw = 0;
#if 0 
	/*
	 * Input validation ensures these warns never fire, but otherwise
	 * suppress unititalized variable usage warnings.
	 */
	if (WARN_ONCE(ways_to_eiw(cxld->interleave_ways, &eiw),
		      "invalid interleave_ways: %d\n", cxld->interleave_ways))
		return;
	if (WARN_ONCE(granularity_to_eig(cxld->interleave_granularity, &eig),
		      "invalid interleave_granularity: %d\n",
		      cxld->interleave_granularity))
		return;
#endif
        u32p_replace_bits(ctrl, eig, CXL_HDM_DECODER0_CTRL_IG_MASK);
        u32p_replace_bits(ctrl, eiw, CXL_HDM_DECODER0_CTRL_IW_MASK);
        *ctrl |= CXL_HDM_DECODER0_CTRL_COMMIT;
}

static void cxld_set_type(u32 *ctrl)
{
        u32p_replace_bits(ctrl, 1, CXL_HDM_DECODER0_CTRL_HOSTONLY);
}
#endif
static int cxl_decoder_commit(struct cxl_hdm *cxl_hdm, int id, u64 base,
                              u64 size)
{
        void *hdm = cxl_hdm->hdm_base;
        /* common decoder settings */
        u32 ctrl = readl(hdm + CXL_HDM_DECODER0_CTRL_OFFSET(id));
        ctrl |= CXL_HDM_DECODER0_CTRL_HOSTONLY;
        kinfo("[CXL] hdm = 0x%llx, base = 0x%llx, size = 0x%llx\n",
              hdm,
              base,
              size);

        writel(upper_32_bits(base),
               hdm + CXL_HDM_DECODER0_BASE_HIGH_OFFSET(id));
        writel(lower_32_bits(base), hdm + CXL_HDM_DECODER0_BASE_LOW_OFFSET(id));
        writel(upper_32_bits(size),
               hdm + CXL_HDM_DECODER0_SIZE_HIGH_OFFSET(id));
        writel(lower_32_bits(size), hdm + CXL_HDM_DECODER0_SIZE_LOW_OFFSET(id));

        void *sk_hi = hdm + CXL_HDM_DECODER0_SKIP_HIGH(id);
        void *sk_lo = hdm + CXL_HDM_DECODER0_SKIP_LOW(id);

        writel(upper_32_bits(0), sk_hi);
        writel(lower_32_bits(0), sk_lo);

#if 0
        writel(ctrl, hdm + CXL_HDM_DECODER0_CTRL_OFFSET(id));
        up_read(&cxl_dpa_rwsem);
        port->commit_end++;
        rc = cxld_await_commit(hdm, cxld->id);
err:
        if (rc) {
                dev_dbg(&port->dev,
                        "%s: error %d committing decoder\n",
                        dev_name(&cxld->dev),
                        rc);
                cxld->reset(cxld);
                return rc;
        }
        cxld->flags |= CXL_DECODER_F_ENABLE;

#endif
        ctrl |= CXL_HDM_DECODER0_CTRL_COMMIT;
        // ctrl &= ~CXL_HDM_DECODER0_CTRL_COMMITTED;
        writel(ctrl, hdm + CXL_HDM_DECODER0_CTRL_OFFSET(id));
        int rc = cxld_await_commit(hdm, id);
        if (rc) {
                BUG("[CXL] cxld_await_commit failed (r=%d)\n", rc);
        }
        return 0;
}

/**
 * cxl_probe_component_regs() - Detect CXL Component register blocks
 * @base: Mapping containing the HDM Decoder Capability Header
 *
 * See CXL 2.0 8.2.4 Component Register Layout and Definition
 * See CXL 2.0 8.2.5.5 CXL Device Register Interface
 *
 * Probe for component register information and return it in map object.
 */
void cxl_probe_component_regs(void *base)
{
        int cap, cap_count;
        u32 cap_array;

        /*
         * CXL.cache and CXL.mem registers are at offset 0x1000 as defined in
         * CXL 2.0 8.2.4 Table 141.
         */
        base += CXL_CM_OFFSET;

        cap_array = readl(base + CXL_CM_CAP_HDR_OFFSET);

        if (FIELD_GET(CXL_CM_CAP_HDR_ID_MASK, cap_array) != CM_CAP_HDR_CAP_ID) {
                kdebug("[CXL] Couldn't locate the CXL.cache and CXL.mem capability array header.\n");
                return;
        }
        if (FIELD_GET(CXL_CM_CAP_HDR_VERSION_MASK, cap_array)
            != CM_CAP_HDR_CAP_VERSION) {
                kdebug("[CXL] Couldn't locate the CXL.cache and CXL.mem capability array header.\n");
                return;
        }

        /* It's assumed that future versions will be backward compatible */
        cap_count = FIELD_GET(CXL_CM_CAP_HDR_ARRAY_SIZE_MASK, cap_array);
        for (cap = 1; cap <= cap_count; cap++) {
                void *register_block;
                u16 cap_id, offset;
                u32 hdr;

                hdr = readl(base + cap * 0x4);

                cap_id = FIELD_GET(CXL_CM_CAP_HDR_ID_MASK, hdr);
                offset = FIELD_GET(CXL_CM_CAP_PTR_MASK, hdr);
                register_block = base + offset;

                kdebug("[CXL] base + cxp * 0x4=%llx, cap_id=%d, offset = %d\n",
                       (u64)(base + cap * 0x4),
                       cap_id,
                       offset);

                switch (cap_id) {
                case CXL_CM_CAP_CAP_ID_HDM: {
                        kinfo("[CXL] found HDM decoder capability (0x%x)\n",
                              offset);
                        parse_hdm_decoder_caps((void *)register_block,
                                               &cxl_hdm);
                        enable_hdm_decoder(register_block);
                        cxl_decoder_commit(&cxl_hdm,
                                           0,
                                           cxl_mem_devs[0].start,
                                           cxl_mem_devs[0].size);
                        break;
                }
                case CXL_CM_CAP_CAP_ID_RAS:
                        kinfo("[CXL] found RAS capability (0x%x)\n", offset);
                        break;
                default:
                        kdebug("[CXL] Unknown CM cap ID: %d (0x%x)\n",
                               cap_id,
                               offset);
                        break;
                }
        }
}

/**
 * CHBS->base: CXL Subsystem Component Register Ranges
 * + CXL_CM_OFFSET: CXL.cache and CXL.mem Architectural Register header
 * + CXL_CM_CAP_HDR_OFFSET: CXL Capability Header
 */

static void cxl_parse_chbs(struct acpi_cedt_chbs *chbs)
{
        struct cxl_chbs_context *ctx;
        ctx = &(cxl_chbs_ctxs[cxl_chbs_ctxs_num++]);

        ctx->uid = chbs->uid;
        ctx->cxl_version = chbs->cxl_version;
        BUG_ON(ctx->cxl_version != ACPI_CEDT_CHBS_VERSION_CXL20);
        ctx->base = chbs->base;
}

static void cxl_parse_cfmws(struct acpi_cedt_cfmws *cfmws)
{
        int r;
        unsigned int ways, ig;

        // verify the CFMWS device
        r = cxl_acpi_cfmws_verify(cfmws);

        if (r) {
                kdebug("CFMWS range %#llx-%#llx not registered\n",
                       cfmws->base_hpa,
                       cfmws->base_hpa + cfmws->window_size - 1);
                return;
        }

        r = eiw_to_ways(cfmws->interleave_ways, &ways);
        if (r) {
                return;
        }

        r = eig_to_granularity(cfmws->granularity, &ig);
        if (r) {
                return;
        }

        kinfo("[CXL] interleave_ways: %d, granularity: %d\n",
              cfmws->interleave_ways,
              cfmws->granularity);

        // for (i = 0; i < ways; i++)
        // target_map[i] = cfmws->interleave_targets[i];

        // store cfmws struct
        if (ways == 1) {
                kinfo("[CXL] interleave_targets[0]: %d\n",
                      cfmws->interleave_targets[0]);

                cxl_mem_devs[cxl_mem_dev_num] = (struct cxl_mem_dev){
                        .start = cfmws->base_hpa, .size = cfmws->window_size};

                kinfo("[CEDT INFO] [CFMWS] range 0x%llx, size 0x%llx\n",
                      cxl_mem_devs[cxl_mem_dev_num].start,
                      cxl_mem_devs[cxl_mem_dev_num].size);
                cxl_mem_dev_num++;
        } else {
                BUG("ChCore currently does not support eniw > 0\n");
        }
}

void parse_cedt(struct acpi_table_cedt *table)
{
        u32 table_len;
        struct acpi_cedt_header *entry;
        struct acpi_cedt_chbs *chbs;
        struct acpi_cedt_cfmws *cfmws;

        // get cedt struct header
        entry = (struct acpi_cedt_header *)GET_TABLE_ENTRY(table);
        table_len = table->header.length;

        while ((char *)entry + entry->length <= (char *)table + table_len) {
                kdebug("[CEDT INFO] entry->entry_type=%d, entry->entry_len=%d\n",
                       entry->type,
                       entry->length);
                if (entry->length == 0)
                        break;

                switch (entry->type) {
                case ACPI_CEDT_TYPE_CHBS: {
                        chbs = (struct acpi_cedt_chbs *)entry;
                        cxl_parse_chbs(chbs);
                        kinfo("[CEDT INFO] [CHBS] ID %d, base 0x%llx\n",
                              chbs->uid,
                              chbs->base);
                        break;
                }
                case ACPI_CEDT_TYPE_CFMWS: {
                        cfmws = (struct acpi_cedt_cfmws *)entry;
                        cxl_parse_cfmws(cfmws);
                        break;
                }
                default:
                        kwarn("Type%d CXL device is currently not supported\n",
                              entry->type);
                }

                entry = (struct acpi_cedt_header *)((char *)entry
                                                    + entry->length);
        }
}
