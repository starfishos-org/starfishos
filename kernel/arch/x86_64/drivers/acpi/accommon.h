#pragma once
/*
 * Flexible array members are not allowed to be part of a union under
 * C99, but this is not for any technical reason. Work around the
 * limitation.
 */
#define ACPI_FLEX_ARRAY(TYPE, NAME) \
    struct {                        \
        struct {                    \
        } __Empty_##NAME;           \
        TYPE NAME[];                \
    }
