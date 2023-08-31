#include "foo.h"
#include "minunit.h"

MU_TEST(test_check) {
	mu_check(5 == 7);
}

MU_TEST(test_add) {
	mu_assert(add(1, 2) == 3, "add fail");
}

MU_TEST(test_sub) {
	mu_assert_int_eq(5, sub(3, -2));
}

MU_TEST_SUITE(test_suite) {
	MU_RUN_TEST(test_check);
	MU_RUN_TEST(test_add);
	MU_RUN_TEST(test_sub);
}

int main(void) {
	MU_RUN_SUITE(test_suite);
	MU_REPORT();
	return minunit_fail;
}
