#pragma once

struct process_metadata {
	u64 phdr_addr;
	u64 phentsize;
	u64 phnum;
	u64 flags;
	u64 entry;
};

#define ENV_SIZE_ON_STACK 0x1000
