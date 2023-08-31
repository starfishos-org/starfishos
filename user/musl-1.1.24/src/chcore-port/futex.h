#pragma once
#include <time.h>

int chcore_futex(int *uaddr, int futex_op, int val,
		 struct timespec *timeout, int *uaddr2, int val3);
