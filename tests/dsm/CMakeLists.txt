cmake_minimum_required(VERSION 3.14)
project(test_cxl_reclaim_policy C)

add_executable(test_cxl_reclaim_policy test_cxl_reclaim_policy.c)
target_include_directories(test_cxl_reclaim_policy PRIVATE
    ../../kernel/include
    ../../kernel/include/arch/x86_64)
target_compile_options(test_cxl_reclaim_policy PRIVATE -Wall -Werror)

enable_testing()
add_test(cxl_reclaim_policy test_cxl_reclaim_policy)
