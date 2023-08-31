#include <arch/drivers/multiboot2.h>
#include <arch/mmu.h>
#include <common/vars.h>
#include <common/util.h>

#include "madt.h"
#include "nfit.h"

static struct rsdp_t *rsdp;
static struct xsdp_t *xsdp;
static struct xsdt_t *xsdt;
static struct rsdt_t *rsdt;
static struct madt_t *madt;
static struct nfit_t *nfit;

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
	struct acpi_sdt_header *h;

	/* Determine which SDT is being used */
	if (xsdt)
		entries = (xsdt->h.length - sizeof(xsdt->h)) / 8;
	else
		entries = (rsdt->h.length - sizeof(rsdt->h)) / 4;

	for (i = 0; i < entries; i++) {
		if (xsdt) /* Use higher 32 bits */
			h = (struct acpi_sdt_header *)(u64)(xsdt->others[i]);
		else
			h = (struct acpi_sdt_header *)(u64)(rsdt->others[i]);

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

/* Currently, this function only parses MADT */
void parse_acpi_info(void *info)
{
	struct acpi_sdt_header *h;

	xsdp = (struct xsdp_t *)info;
	rsdp = (struct rsdp_t *)info;

	/* validate RSDP */
	if (!do_checksum((char *)rsdp, sizeof(*rsdp)))
		BUG("RSDP checksum invalid\n");

	if (rsdp->revision == 0) {
		/* ACPI version = 1.0, use rsdp and rsdt */
		kinfo("[ACPI INFO] ACPI version 1.0, use rsdp and rsdt\n");
		h = (struct acpi_sdt_header *)(u64)phys_to_virt(rsdp->rsdt_addr);
		rsdt = (struct rsdt_t *)h;
	} else {
		/* ACPI version >= 2.0, use xsdp and xsdt */
		kinfo("[ACPI INFO] ACPI version >= 2.0, use xsdp and xsdt\n");

		/* validate XSDP */
		if (!do_checksum((char *)xsdp, xsdp->length))
			BUG("XSDP checksum invalid\n");

		h = (struct acpi_sdt_header *)phys_to_virt(xsdp->xsdt_addr);
		xsdt = (struct xsdt_t *)h;
	}

	/* validate SDT */
	if (!do_checksum((char *)h, h->length))
		BUG("SDT checksum invalid\n");

	/* find MADT using its signature "APIC" */
	madt = (struct madt_t *)find_table_by_sig("APIC");
	if (!madt)
		BUG("MADT not found\n");
	madt = (struct madt_t *)phys_to_virt(madt);

	/* parse MADT */
	parse_madt(madt);

	nfit = (struct nfit_t *)find_table_by_sig("NFIT");
	if (!nfit)
		BUG("NFIT not found\n");
	nfit = (struct nfit_t *)phys_to_virt(nfit);

	/* parse NFIT */
	parse_nfit(nfit);
}
