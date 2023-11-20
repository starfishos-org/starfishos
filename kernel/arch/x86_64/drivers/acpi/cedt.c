// reference: linux-6.18/drivers/cxl/acpi.c
#include <common/util.h>
#include <common/macro.h>
#include <common/types.h>
#include <common/size.h>
#include <drivers/cxl.h>

#include "cedt.h"

extern struct cxl_mem_dev cxl_mem_devs[12];
extern int cxl_mem_dev_num;

static int cxl_acpi_cfmws_verify(struct cedt_cfmws *cfmws)
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

static void cxl_parse_cfmws(struct cedt_cfmws *cfmws)
{
        int r;

        // verify the CFMWS device
        r = cxl_acpi_cfmws_verify(cfmws);

        if (r) {
                kdebug("CFMWS range %#llx-%#llx not registered\n",
                       cfmws->base_hpa,
                       cfmws->base_hpa + cfmws->window_size - 1);
                return;
        }

        // store cfmws struct
        if (cfmws->eniw == 0) {
                cxl_mem_devs[cxl_mem_dev_num] = (struct cxl_mem_dev){
                        .start = cfmws->base_hpa, .size = cfmws->window_size};

                kinfo("[CEDT INFO] [CFMWS] range 0x%llx, size 0x%llx\n",
                      cxl_mem_devs[cxl_mem_dev_num].start,
                      cxl_mem_devs[cxl_mem_dev_num].size);
                cxl_mem_dev_num++;
        }
}

void parse_cedt(struct cedt_t *cedt)
{
        u32 cedt_len;
        struct cedt_entry_header *entry;
        struct cedt_chbs *chbs;
        struct cedt_cfmws *cfmws;

        // get cedt struct header
        entry = (struct cedt_entry_header *)cedt->entries;
        cedt_len = cedt->h.length;

        while ((char *)entry + entry->entry_len <= (char *)cedt + cedt_len) {
                kdebug("[CEDT INFO] entry->entry_type=%d, entry->entry_len=%d\n",
                       entry->entry_type,
                       entry->entry_len);
                if (entry->entry_len == 0)
                        break;

                switch (entry->entry_type) {
                case ACPI_CXL_HOST_BRIDGE_STRUCTURE: {
                        chbs = (struct cedt_chbs *)entry;
                        kinfo("[CEDT INFO] [CHBS] ID %d\n", chbs->uid);
                        break;
                }
                case ACPI_CXL_FIXED_MEMORY_WINDOW_STRUCTURE: {
                        cfmws = (struct cedt_cfmws *)entry;
                        cxl_parse_cfmws(cfmws);
                        break;
                }
                default:
                        kwarn("Type%d CXL device is currently not supported\n",
                              entry->entry_type);
                }

                entry = (struct cedt_entry_header *)((char *)entry
                                                     + entry->entry_len);
        }
}
