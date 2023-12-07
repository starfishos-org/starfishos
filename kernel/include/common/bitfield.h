#pragma once

#define __bf_shf(x) (__builtin_ffsll(x) - 1)
/**
 * FIELD_GET() - extract a bitfield element
 * @_mask: shifted mask defining the field's length and position
 * @_reg:  value of entire bitfield
 *
 * FIELD_GET() extracts the field specified by @_mask from the
 * bitfield passed in as @_reg by masking and shifting it down.
 */
#define FIELD_GET(_mask, _reg) \
        ({ (typeof(_mask))(((_reg) & (_mask)) >> __bf_shf(_mask)); })
