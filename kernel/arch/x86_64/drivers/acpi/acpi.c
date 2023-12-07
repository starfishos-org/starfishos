#include "actbl.h"
#include "common/types.h"
#include <arch/drivers/multiboot2.h>
#include <arch/mmu.h>
#include <common/vars.h>
#include <common/util.h>

#include "acpi.h"

static struct acpi_table_rsdp *rsdp;
static struct acpi_table_xsdt *xsdt;
static struct acpi_table_rsdt *rsdt;

static int do_checksum(char *start, u64 len)
{
        u64 i;
        char sum;

        sum = 0;

        for (i = 0; i < len; i++)
                sum += start[i];

        return sum == 0;
}

static void *find_table_by_sig(char *sig)
{
        int entries, i;
        struct acpi_table_header *h;

        /* Determine which SDT is being used */
        if (xsdt)
                entries = (xsdt->header.length - sizeof(xsdt->header))
                          / ACPI_XSDT_ENTRY_SIZE;
        else
                entries = (rsdt->header.length - sizeof(rsdt->header))
                          / ACPI_RSDT_ENTRY_SIZE;

        for (i = 0; i < entries; i++) {
                if (xsdt) /* Use higher 32 bits */
                        h = (struct acpi_table_header
                                     *)(xsdt->table_offset_entry[i]);
                else
                        h = (struct acpi_table_header
                                     *)(u64)(rsdt->table_offset_entry[i]);

                /* Table name only consists of 4 characters */
                if (!strncmp(h->signature, sig, 4)) {
                        /* validate checksum */
                        if (!do_checksum((char *)h, h->length))
                                continue;
                        return (void *)h;
                }
        }

        /* Table not found */
        return NULL;
}

#ifdef USE_CXL_MEM
ACPI_BUILD_PARSE_TABLE(cedt, ACPI_SIG_CEDT)
#endif
ACPI_BUILD_PARSE_TABLE(madt, ACPI_SIG_MADT)
ACPI_BUILD_PARSE_TABLE(nfit, ACPI_SIG_NFIT)
ACPI_BUILD_PARSE_TABLE(srat, ACPI_SIG_SRAT)
ACPI_BUILD_PARSE_TABLE(mcfg, ACPI_SIG_MCFG)

/* Currently, this function only parses MADT */
void parse_acpi_info(void *info)
{
        struct acpi_table_header *h;

        rsdp = (struct acpi_table_rsdp *)info;
        if (rsdp->revision == 0) {
                /* ACPI version = 1.0, use rsdp and rsdt */
                /* validate RSDP */
                if (!do_checksum((char *)rsdp, sizeof(struct acpi_rsdp_common)))
                        BUG("RSDP checksum invalid\n");

                kinfo("[ACPI INFO] ACPI version 1.0, use COMMON RSDP and RSDT\n");
                h = (struct acpi_table_header *)phys_to_virt(
                        (void *)(u64)rsdp->rsdt_physical_address);
                rsdt = (struct acpi_table_rsdt *)h;
        } else if (rsdp->revision == 2) {
                /* ACPI version >= 2.0, use extented rsdp and xsdt */

                /* validate RSDP */
                if (!do_checksum((char *)rsdp, sizeof(struct acpi_table_rsdp)))
                        BUG("RSDP checksum invalid\n");

                kinfo("[ACPI INFO] ACPI version >= 2.0, use extented RSDP and XSDT\n");
                h = (struct acpi_table_header *)phys_to_virt(
                        rsdp->xsdt_physical_address);
                xsdt = (struct acpi_table_xsdt *)h;
        } else {
                BUG("[ACPI] Unsupport RSDP revision\n");
        }

        /* validate SDT */
        if (!do_checksum((char *)h, h->length))
                BUG("SDT checksum invalid\n");

#ifdef USE_CXL_MEM
        /* parse CEDT */
        acpi_parse_cedt();
#endif
        /* find and parse MADT */
        acpi_parse_madt();

        /* find and parse NFIT */
        acpi_parse_nfit();

        /* find and parse SRAT */
        acpi_parse_srat();
}

void acpi_parse_pci_info()
{
        /* XSDT or RSDT should be parsed */
        BUG_ON(!xsdt && !rsdt);

        /* find and parse MCFG */
        acpi_parse_mcfg();
}
