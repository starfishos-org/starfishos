chcore_config(CHCORE_BUILD_SAMPLE_APPS_APPS BOOL ON "Build sample apps?")
chcore_config(CHCORE_BUILD_SAMPLE_APPS_UTILS BOOL ON "Build sample utils?")
chcore_config(CHCORE_BUILD_SAMPLE_APPS_TESTS BOOL ON "Build sample tests?")
chcore_config(CHCORE_BUILD_USER_MALLOC_TESTS BOOL ON
    "User-space malloc benchmarks: perf_libc_malloc.bin, malloc_benchmark.bin (needs sample tests ON)")
