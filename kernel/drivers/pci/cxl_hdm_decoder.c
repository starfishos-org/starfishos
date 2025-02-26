#include <arch/time.h>
#include <drivers/pci.h>
#include <drivers/cxl.h>
#include <common/util.h>
#include <common/macro.h>
#include <common/types.h>
#include <common/size.h>
#include <common/bitfield.h>
#include <arch/io.h>
#include <arch/drivers/cxl.h>

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
        cxl_debug(
                "[CXL] base: 0x%llx, decoder cnt: %d, target_count: %d, interleave_mask: %d\n",
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

        for (i = 0; i < COMMIT_TIMEOUT_MS; i++) {
                ctrl = readl(hdm + CXL_HDM_DECODER0_CTRL_OFFSET(id));
                if (FIELD_GET(CXL_HDM_DECODER0_CTRL_COMMIT_ERROR, ctrl)) {
                        ctrl &= CXL_HDM_DECODER0_CTRL_COMMIT;
                        writel(ctrl, hdm + CXL_HDM_DECODER0_CTRL_OFFSET(id));
                        return -100;
                }
                if (FIELD_GET(CXL_HDM_DECODER0_CTRL_COMMITTED, ctrl))
                        return 0;
                delay_ms(COMMIT_TIMEOUT_MS);
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
        cxl_debug("[CXL] hdm = 0x%llx, base = 0x%llx, size = 0x%llx\n",
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

        ctrl |= CXL_HDM_DECODER0_CTRL_COMMIT;
        // ctrl &= ~CXL_HDM_DECODER0_CTRL_COMMITTED;
        writel(ctrl, hdm + CXL_HDM_DECODER0_CTRL_OFFSET(id));
        int rc = cxld_await_commit(hdm, id);
        if (rc) {
                cxl_error("[CXL] cxld_await_commit failed (r=%d)\n", rc);
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
void cxl_probe_component_regs(struct pci_dev *dev, void *base,
                              struct cxl_component_reg_map *map)
{
        int cap, cap_count;
        u32 cap_array;

        *map = (struct cxl_component_reg_map){0};

        /*
         * CXL.cache and CXL.mem registers are at offset 0x1000 as defined in
         * CXL 2.0 8.2.4 Table 141.
         */
        base += CXL_CM_OFFSET;

        cap_array = readl(base + CXL_CM_CAP_HDR_OFFSET);

        if (FIELD_GET(CXL_CM_CAP_HDR_ID_MASK, cap_array) != CM_CAP_HDR_CAP_ID) {
                cxl_error(
                        "[CXL] Couldn't locate the CXL.cache and CXL.mem capability array header.\n");
                return;
        }
        if (FIELD_GET(CXL_CM_CAP_HDR_VERSION_MASK, cap_array)
            != CM_CAP_HDR_CAP_VERSION) {
                cxl_error(
                        "[CXL] Couldn't locate the CXL.cache and CXL.mem capability array header.\n");
                return;
        }

        /* It's assumed that future versions will be backward compatible */
        cap_count = FIELD_GET(CXL_CM_CAP_HDR_ARRAY_SIZE_MASK, cap_array);
        for (cap = 1; cap <= cap_count; cap++) {
                void *register_block;
                struct cxl_reg_map *rmap;
                u16 cap_id, offset;
                u32 length, hdr;

                hdr = readl(base + cap * 0x4);

                cap_id = FIELD_GET(CXL_CM_CAP_HDR_ID_MASK, hdr);
                offset = FIELD_GET(CXL_CM_CAP_PTR_MASK, hdr);
                register_block = base + offset;
                hdr = readl(register_block);

                rmap = NULL;
                cxl_debug(
                        "[CXL] base + cxp * 0x4=%llx, cap_id=%d, offset = %d\n",
                        (u64)(base + cap * 0x4),
                        cap_id,
                        offset);

                switch (cap_id) {
                case CXL_CM_CAP_CAP_ID_HDM: {
                        int decoder_cnt;

                        cxl_debug("[CXL] found HDM decoder capability (0x%x)\n",
                                  offset);

                        decoder_cnt = cxl_hdm_decoder_count(hdr);
                        length = 0x20 * decoder_cnt + 0x10;
                        rmap = &map->hdm_decoder;

                        /* TODO: ugly directly use hdm decode here */
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
                        cxl_debug("[CXL] found RAS capability (0x%x)\n",
                                  offset);
                        break;
                default:
                        cxl_debug("[CXL] Unknown CM cap ID: %d (0x%x)\n",
                                  cap_id,
                                  offset);
                        break;
                }

                if (!rmap)
                        continue;
                rmap->valid = true;
                rmap->id = cap_id;
                rmap->offset = CXL_CM_OFFSET + offset;
                rmap->size = length;
        }
}

/**
 * cxl_probe_device_regs() - Detect CXL Device register blocks
 * @dev: Host device of the @base mapping
 * @base: Mapping of CXL 2.0 8.2.8 CXL Device Register Interface
 * @map: Map object describing the register block information found
 *
 * Probe for device register information and return it in map object.
 */
void cxl_probe_device_regs(struct pci_dev *dev, void *base,
                           struct cxl_device_reg_map *map)
{
        int cap, cap_count;
        u64 cap_array;

        *map = (struct cxl_device_reg_map){0};

        cap_array = readq(base + CXLDEV_CAP_ARRAY_OFFSET);
        if (FIELD_GET(CXLDEV_CAP_ARRAY_ID_MASK, cap_array)
            != CXLDEV_CAP_ARRAY_CAP_ID)
                return;

        cap_count = FIELD_GET(CXLDEV_CAP_ARRAY_COUNT_MASK, cap_array);

        for (cap = 1; cap <= cap_count; cap++) {
                struct cxl_reg_map *rmap;
                u32 offset, length;
                u16 cap_id;

                cap_id = FIELD_GET(CXLDEV_CAP_HDR_CAP_ID_MASK,
                                   readl(base + cap * 0x10));
                offset = readl(base + cap * 0x10 + 0x4);
                length = readl(base + cap * 0x10 + 0x8);

                rmap = NULL;
                switch (cap_id) {
                case CXLDEV_CAP_CAP_ID_DEVICE_STATUS:
                        cxl_debug("found Status capability (0x%x)\n", offset);
                        rmap = &map->status;
                        break;
                case CXLDEV_CAP_CAP_ID_PRIMARY_MAILBOX:
                        cxl_debug("found Mailbox capability (0x%x)\n", offset);
                        rmap = &map->mbox;
                        break;
                case CXLDEV_CAP_CAP_ID_SECONDARY_MAILBOX:
                        cxl_debug("found Secondary Mailbox capability (0x%x)\n",
                                  offset);
                        break;
                case CXLDEV_CAP_CAP_ID_MEMDEV:
                        cxl_debug("found Memory Device capability (0x%x)\n",
                                  offset);
                        rmap = &map->memdev;
                        break;
                default:
                        if (cap_id >= 0x8000) {
                                cxl_debug("Vendor cap ID: %#x offset: %#x\n",
                                          cap_id,
                                          offset);
                        } else {
                                cxl_error("Unknown cap ID: %#x offset: %#x\n",
                                          cap_id,
                                          offset);
                        }
                        break;
                }

                if (!rmap)
                        continue;
                rmap->valid = true;
                rmap->id = cap_id;
                rmap->offset = offset;
                rmap->size = length;
        }
}
