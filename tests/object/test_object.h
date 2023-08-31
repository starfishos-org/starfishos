#pragma once

#include <object/object.h>

enum test_object_type {
	TYPE_TEST_OBJECT = TYPE_NR,
	TYPE_TEST_NR
};

struct test_object {
	u64 test_int;
};

int test_object_invoke(struct test_object *test_object, int invlabel,
		       u64 *args, u64 rights);
int sys_test_object_set_int(u64 cap, u64 arg);
int sys_test_object_get_int(u64 cap);
