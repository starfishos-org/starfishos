#pragma once
#include "minunit.h"

#define mu_assert_msg(test, format, arg...) MU__SAFE_BLOCK(\
	minunit_assert++;\
	if (!(test)) {\
		snprintf(minunit_last_message, MINUNIT_MESSAGE_LEN,\
			 "%s failed:\n\t%s:%d: " format,\
			 __func__, __FILE__, __LINE__, ##arg);\
		minunit_status = 1;\
		return;\
	}\
)
