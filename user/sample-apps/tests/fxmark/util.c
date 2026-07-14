// SPDX-License-Identifier: MIT
#include <stdio.h>
#include <stdlib.h>
#include <limits.h>
#include <sys/stat.h>

int mkdir_p(const char *path)
{
    int ret = mkdir(path, 0755);
    if (ret != 0) {
        return ret;
    }
    return 0; // success
}
