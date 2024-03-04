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
                kdebug("CFMWS Window Size not 256MB aligned\n");
                return -1;
        }

        // TODO: add more verify entries

        return 0;
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
        // BUG_ON(ctx->cxl_version != ACPI_CEDT_CHBS_VERSION_CXL20);
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
