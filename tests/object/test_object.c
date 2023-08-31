#include <stdio.h>
#include <string.h>
#include "test_object.h"
#include <object/cap_group.h>
#include <object/thread.h>


int sys_test_object_set_int(u64 cap, u64 arg)
{
	struct test_object *test_obj;
	test_obj = obj_get(current_cap_group, cap, TYPE_TEST_OBJECT);
	if (!test_obj) {
		return -ECAPBILITY;
	}
	test_obj->test_int = arg;
	printf("set test_int to 0x%llx\n", test_obj->test_int);
	obj_put(test_obj);
	return 0;
}

int sys_test_object_get_int(u64 cap)
{
	struct test_object *test_obj;
	test_obj = obj_get(current_cap_group, cap, TYPE_TEST_OBJECT);
	if (!test_obj) {
		return -ECAPBILITY;
	}
	printf("get test_int 0x%llx\n", test_obj->test_int);
	obj_put(test_obj);
	return 0;
}
