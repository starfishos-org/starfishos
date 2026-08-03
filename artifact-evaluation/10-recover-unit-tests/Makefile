CC ?= cc
CFLAGS ?= -O2 -g -std=c11 -Wall -Wextra -Werror
POLLING_DIR := ../../user/system-servers/polling
TEST_CPPFLAGS := -I. -I$(POLLING_DIR) -include test_hooks.h

.PHONY: all run clean

all: durable_queue_recovery_test

durable_queue_recovery_test: durable_queue_recovery_test.c test_hooks.h fs_wrapper_defs.h \
		$(POLLING_DIR)/polling_req.c $(POLLING_DIR)/polling.h
	$(CC) $(CFLAGS) $(TEST_CPPFLAGS) durable_queue_recovery_test.c \
		$(POLLING_DIR)/polling_req.c -o $@

run: durable_queue_recovery_test
	./durable_queue_recovery_test

clean:
	rm -f durable_queue_recovery_test
