#include "child.h"

bool child_inc(child_t *self, size_t order)
{
	u32 num_pages = (u32)(1u << order);
	if (self->huge || (u32)(self->free + num_pages) > (u32)LLFREE_CHILD_SIZE)
		return false;

	self->free += num_pages;
	return true;
}

bool child_dec(child_t *self, size_t order)
{
	u32 num_pages = (u32)(1u << order);
	if (!self->huge && self->free >= num_pages) {
		self->free -= num_pages;
		return true;
	}
	return false;
}

bool child_set_huge(child_t *self)
{
	if (self->free == (u32)LLFREE_CHILD_SIZE) {
		llfree_assert(!self->huge);
		*self = child_new(0, true);
		return true;
	}
	return false;
}

bool child_clear_huge(child_t *self)
{
	if (self->huge) {
		llfree_assert(self->free == 0);
		*self = child_new(LLFREE_CHILD_SIZE, false);
		return true;
	}
	return false;
}

bool child_set_max(child_pair_t *self)
{
	if (self->first.free == (u32)LLFREE_CHILD_SIZE &&
	    self->second.free == (u32)LLFREE_CHILD_SIZE) {
		llfree_assert(!self->first.huge && !self->second.huge);
		self->first = child_new(0, true);
		self->second = child_new(0, true);
		return true;
	}
	return false;
}

bool child_clear_max(child_pair_t *self)
{
	if (self->first.huge && self->second.huge) {
		llfree_assert(self->first.free == 0 && self->second.free == 0);
		self->first = child_new(LLFREE_CHILD_SIZE, false);
		self->second = child_new(LLFREE_CHILD_SIZE, false);
		return true;
	}
	return false;
}

