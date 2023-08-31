#pragma once

#define CHCORE_IS_ERR(x) (((long long)(x)) < 0)
#define CHCORE_ERR_PTR(x) ((void *)(long long)(x))
#define CHCORE_PTR_ERR(x) ((long)(x))
