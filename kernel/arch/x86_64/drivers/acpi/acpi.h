#pragma once
#include <common/types.h>

#include "actbl3.h"

#define GET_TABLE_ENTRY(table) (void *)((u64)table + sizeof(*table))

#define ACPI_PARSE_TABLE(table, sig)                                 \
    struct acpi_table_##table *t =                                   \
            (struct acpi_table_##table *)find_table_by_sig(sig);     \
    if (!t) {                                                        \
        kinfo("[ACPI] [%s] table not found\n", sig);                 \
    } else {                                                         \
        parse_##table((struct acpi_table_##table *)phys_to_virt(t)); \
    }

#define ACPI_BUILD_PARSE_TABLE(table, sig)                               \
    static inline void acpi_parse_##table()                              \
    {                                                                    \
        struct acpi_table_##table *t =                                   \
                (struct acpi_table_##table *)find_table_by_sig(sig);     \
        if (!t) {                                                        \
            kinfo("[ACPI] [%s] table not found\n", sig);                 \
        } else {                                                         \
            parse_##table((struct acpi_table_##table *)phys_to_virt(t)); \
        }                                                                \
    }

void parse_acpi_info(void *info);
void acpi_parse_pci_info();

void parse_madt(struct acpi_table_madt *);
void parse_cedt(struct acpi_table_cedt *);
void parse_srat(struct acpi_table_srat *);
void parse_nfit(struct acpi_table_nfit *);
void parse_mcfg(struct acpi_table_mcfg *);
