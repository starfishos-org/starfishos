#pragma once

#include <sys/eventfd.h>

int chcore_eventfd(unsigned int count, int flags);