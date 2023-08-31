long chcore_x86_set_thread_area(long fs)
{
	/*
	 * TODO: wrfsbase may not be supported.
	 * If so, use system call for executing wrmsr instead.
	 */
	__asm__ __volatile__("wrfsbase %0"::"r"(fs));
	return 0;
}
