#include <string.h>
#include <object/object.h>
#include <object/thread.h>
#include <object/cap_group.h>
#include "minunit_local.h"
#include "test_object.h"

u64 virt_to_phys(u64 x)
{
	return x;
}

u64 phys_to_virt(u64 x)
{
	return x;
}

#define BUG_MSG_ON(condition, format, arg...)   \
	do {                                    \
		if (condition) {                \
			printf(format, ##arg);  \
			BUG();                  \
		}                               \
	} while (0)

#define UNUSED(x) ((void)x)

int sys_create_cap_group(u64 badge, u64 cap_group_name, u64 name_len, u64 pcid);

struct thread_args {
	u64 cap_group_cap;
	u64 stack;
	u64 pc;
	u64 arg;
	u32 prio;
	u64 tls;
	u32 type;
};
int sys_create_thread(u64 thread_args_p);
int sys_get_all_caps(u64 cap_group_cap);
struct thread *create_user_thread_stub(struct cap_group *cap_group);

static void test_expand(struct cap_group *cap_group)
{
	int i, cap_tmp, cap_lowest;
	void *obj;

#define TOTAL_ALLOC	200
	for (i = 0; i < TOTAL_ALLOC; i++) {
		obj = obj_alloc(TYPE_TEST_OBJECT, sizeof(struct test_object));
		cap_tmp = cap_alloc(cap_group, obj, 0);
		/* printf("\tinit multiple test_object cap = %d\n", cap_tmp); */
		if (i == 0) {
			cap_lowest = cap_tmp;
		} else {
			mu_assert_msg(cap_tmp == cap_lowest + i,
				      "slot id not increasing sequentially\n");
		}
	}
	printf("alloc from %d to %d\n", cap_lowest,
	       cap_lowest + TOTAL_ALLOC - 1);

#define FREE_OFFSET	60
#define TOTAL_FREE	30
	for (i = FREE_OFFSET; i < FREE_OFFSET + TOTAL_FREE; i++)
		cap_free(cap_group, i);
	printf("free from %d to %d\n", FREE_OFFSET,
	       FREE_OFFSET + TOTAL_FREE - 1);

	for (i = 0; i < TOTAL_FREE; i++) {
		obj = obj_alloc(TYPE_TEST_OBJECT, sizeof(struct test_object));
		cap_tmp = cap_alloc(cap_group, obj, 0);
		mu_assert_msg(cap_tmp == FREE_OFFSET + i,
			      "create test_object failed, ret = %d\n", cap_tmp);
	}
	printf("alloc from %d to %d\n", FREE_OFFSET,
	       FREE_OFFSET + TOTAL_FREE - 1);

}

MU_TEST(test_check)
{
	int ret, cap_child_cap_group, cap_thread_child;
	int cap, cap_copied;
	struct thread *child_thread, *root_thread;
	struct cap_group *child_cap_group, *root_cap_group;
	struct test_object *test_object;
	struct object *object;
	void *obj;
	struct thread_args args = {0};

	/* create root cap_group */
	root_cap_group = create_root_cap_group("test", 4);
	mu_assert_msg(root_cap_group != NULL, "create_root_cap_group failed\n");

	/* create root thread */
	root_thread = create_user_thread_stub(root_cap_group);
	mu_assert_msg(root_thread != NULL, "create_user_thread failed\n");

	/* root_thread create child cap_group */
	current_thread = root_thread;
	ret = sys_create_cap_group(0x1024llu, (u64)"test", 4, 0);
	mu_assert_msg(ret >= 0, "sys_create_cap_group failed, ret = %d\n", ret);
	cap_child_cap_group = ret;
	child_cap_group = obj_get(root_cap_group, cap_child_cap_group,
				  TYPE_CAP_GROUP);

	/* create thread child cap_group*/
	args.cap_group_cap = cap_child_cap_group;
	ret = sys_create_thread((u64)&args);
	mu_assert_msg(ret >= 0, "sys_create_thread failed, ret = %d\n", ret);
	/* ret: child thread cap in farther process */
	cap_thread_child = ret;
	child_thread = obj_get(root_cap_group, cap_thread_child, TYPE_THREAD);
	mu_assert_msg(child_thread != NULL, "obj_get child_thread failed\n");

#if 0
	printf("cap father\n");
	sys_get_all_caps(0);
	current_thread = child_thread;
	printf("cap child\n");
	sys_get_all_caps(0);
	current_thread = root_thread;
#endif

	/* create object */
	obj = obj_alloc(TYPE_TEST_OBJECT, sizeof(struct test_object));
	ret = cap_alloc(root_cap_group, obj, 0);
	mu_assert_msg(ret >= 0, "create test_object failed, ret = %d\n", ret);
	cap = ret;
	printf("init test_object cap = %d\n", cap);

	/* invoke SET */
	ret = sys_test_object_set_int(cap, 0xdeadbeef);
	mu_assert_msg(ret >= 0,
		      "invoke sys_test_object_set_int failed, ret = %d\n", ret);

	/* copy */
	ret = cap_copy(root_cap_group, root_cap_group, cap);
	mu_assert_msg(ret >= 0, "copy cap failed, ret = %d\n", ret);
	cap_copied = ret;
	printf("copied cap = %d\n", cap_copied);

	/* test expanding slots */
	test_expand(root_cap_group);

	/* free old cap */
	ret = cap_free(root_cap_group, cap);
	mu_assert_msg(ret >= 0, "free cap failed, ret = %d\n", ret);

	/* invoke GET on old cap should failed */
	ret = sys_test_object_get_int(cap);
	mu_assert_msg(ret != 0,
		      "sys_test_object_get_int should failed but ret 0\n");

	/* invoke GET on copied cap */
	ret = sys_test_object_get_int(cap_copied);
	mu_assert_msg(ret >= 0,
		      "sys_test_object_get_int failed, ret = %d\n", ret);
	cap = cap_copied;

	/* copy to child cap_group */
	cap_copied = cap_copy(root_cap_group, child_cap_group, cap);
	mu_assert_msg(cap_copied >= 0,
		      "cap_copy failed, ret = %d\n", cap_copied);

	/* invoke GET on child copied cap */
	current_thread = child_thread;
	ret = sys_test_object_get_int(cap_copied);
	mu_assert_msg(ret >= 0,
		      "sys_test_object_get_int failed, ret = %d\n", ret);

	test_object = obj_get(child_cap_group, cap_copied, TYPE_TEST_OBJECT);

	/* remove all the caps pointed to some object */
	ret = cap_free_all(child_cap_group, cap_copied);
	mu_assert_msg(ret >= 0,
		      "cap_free_all failed, ret = %d\n", ret);
	object = container_of(test_object, struct object, opaque);
	mu_assert_msg(object->refcount == 1, "refcount of obj not 1\n");

	/* invoke on original cap should fail */
	current_thread = root_thread;
	ret = sys_test_object_get_int(cap);
	mu_assert_msg(ret != 0,
		      "sys_test_object_get_int should failed but ret 0\n");

	obj_put(test_object);
}

MU_TEST_SUITE(test_suite) {
	MU_RUN_TEST(test_check);
}

obj_deinit_func obj_test_deinit_tbl[TYPE_TEST_NR] = {
       [0 ... TYPE_TEST_NR - 1] = NULL,
};

static void prepare_obj_deinit_tbl(void)
{
	memcpy(obj_test_deinit_tbl, obj_deinit_tbl, sizeof(obj_deinit_tbl));
}

int main(void)
{
	prepare_obj_deinit_tbl();
	MU_RUN_SUITE(test_suite);
	MU_REPORT();
	return minunit_fail;
}
