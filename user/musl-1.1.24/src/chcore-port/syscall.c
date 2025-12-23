#include <chcore/syscall.h>
#include <chcore/defs.h>
#include <chcore/type.h>
#include <chcore/proc.h>
#include <syscall_arch.h>
#include <errno.h>

#include <stdio.h>

void usys_putc(char ch)
{
	chcore_syscall1(CHCORE_SYS_putc, ch);
}

u32 usys_getc(void)
{
	return (u32) chcore_syscall0(CHCORE_SYS_getc);
}

_Noreturn void usys_exit(u64 ret)
{
	chcore_syscall1(CHCORE_SYS_thread_exit, ret);
	__builtin_unreachable();
}

u64 usys_yield(void)
{
	return chcore_syscall0(CHCORE_SYS_yield);
}

int usys_create_device_pmo(u64 paddr, u64 size)
{
	return chcore_syscall2(CHCORE_SYS_create_device_pmo, paddr, size);
}

int usys_create_pmo(u64 size, u64 type, s32 flags)
{
	return chcore_syscall3(CHCORE_SYS_create_pmo, size, type, flags);
}

int usys_map_pmo(u64 cap_group_cap, u64 pmo_cap, u64 addr, u64 rights)
{
	return chcore_syscall5(CHCORE_SYS_map_pmo,
		cap_group_cap, pmo_cap, addr, rights, -1 /* pmo size */);
}

int usys_unmap_pmo(u64 cap_group_cap, u64 pmo_cap, u64 addr)
{
	return chcore_syscall3(CHCORE_SYS_unmap_pmo, cap_group_cap, pmo_cap, addr);
}

int usys_revoke_cap(u64 obj_cap)
{
	return chcore_syscall1(CHCORE_SYS_revoke_cap, obj_cap);
}

int usys_set_affinity(u64 thread_cap, s32 aff)
{
	return chcore_syscall2(CHCORE_SYS_set_affinity, thread_cap, (u64)aff);
}

s32 usys_get_affinity(u64 thread_cap)
{
	return chcore_syscall1(CHCORE_SYS_get_affinity, thread_cap);
}

int usys_get_pmo_paddr(u64 pmo_cap, u64 *buf)
{
	return chcore_syscall2(CHCORE_SYS_get_pmo_paddr, pmo_cap, (u64)buf);
}

int usys_get_phys_addr(void *vaddr, u64 *paddr)
{
	return chcore_syscall2(CHCORE_SYS_get_phys_addr, (u64)vaddr, (u64)paddr);
}

int usys_create_thread(u64 thread_args_p)
{
	return chcore_syscall1(CHCORE_SYS_create_thread, thread_args_p);
}

int usys_create_cap_group(u64 badge, char *name, u64 name_len, u64 pcid, bool is_cross_machine)
{
	return chcore_syscall5(CHCORE_SYS_create_cap_group, badge, (u64)name,
			       name_len, pcid, is_cross_machine);
}

int usys_register_server(u64 callback, u32 register_thread_cap, u64 destructor)
{
	return chcore_syscall3(CHCORE_SYS_register_server, callback,
		       register_thread_cap, destructor);
}

u32 usys_register_client(u32 server_cap, u64 vm_config_ptr)
{
	return chcore_syscall2(CHCORE_SYS_register_client, server_cap, vm_config_ptr);
}

/* TODO: ipc data transfer through registers */
/* XXX: all args are passed by registers as stack is modified
 * is shared stack is used*/
u64 usys_ipc_call(u32 conn_cap, u64 ipc_msg_ptr, u64 cap_num)
{
	return chcore_syscall3(CHCORE_SYS_ipc_call, conn_cap, ipc_msg_ptr,
			       cap_num);
}

void usys_ipc_return(u64 ret, u64 cap_num)
{
	chcore_syscall2(CHCORE_SYS_ipc_return, ret, cap_num);
}

int usys_ipc_register_cb_return(u64 server_thread_cap, u64 server_thread_exit_routine,
				 u64 server_shm_addr)
{
	return chcore_syscall3(CHCORE_SYS_ipc_register_cb_return, server_thread_cap,
		server_thread_exit_routine, server_shm_addr);
}

u64 usys_ipc_send_cap(u32 conn_cap, u32 send_cap)
{
	return chcore_syscall2(CHCORE_SYS_ipc_send_cap, conn_cap, send_cap);
}

void usys_debug_va(u64 va)
{
	chcore_syscall1(CHCORE_SYS_debug_va, va);
}

int usys_cap_copy_to(u64 dest_cap_group_cap, u64 src_slot_id)
{
	return chcore_syscall2(CHCORE_SYS_cap_copy_to, dest_cap_group_cap, src_slot_id);
}

int usys_cap_copy_from(u64 src_cap_group_cap, u64 src_slot_id)
{
	return chcore_syscall2(CHCORE_SYS_cap_copy_from, src_cap_group_cap, src_slot_id);
}

int usys_create_pmos(void *req, u64 cnt, s32 flags)
{
	return chcore_syscall3(CHCORE_SYS_create_pmos, (u64)req, cnt, flags);
}

int usys_map_pmos(u64 cap, void *req, u64 cnt, s32 flags)
{
	return chcore_syscall4(CHCORE_SYS_map_pmos, cap, (u64)req, cnt, flags);
}

int usys_write_pmo(u64 cap, u64 offset, void *buf, u64 size)
{
	return chcore_syscall4(CHCORE_SYS_write_pmo, cap, offset, (u64)buf, size);
}

int usys_read_pmo(u64 cap, u64 offset, void *buf, u64 size)
{
	return chcore_syscall4(CHCORE_SYS_read_pmo, cap, offset, (u64)buf, size);
}

int usys_transfer_caps(u64 cap_group, int *src_caps, int nr, int *dst_caps)
{
	return chcore_syscall4(CHCORE_SYS_transfer_caps, cap_group, (u64)src_caps,
		       (u64)nr, (u64)dst_caps);
}

void usys_perf_start(void)
{
	chcore_syscall0(CHCORE_SYS_perf_start);
}

void usys_perf_end(void)
{
	chcore_syscall0(CHCORE_SYS_perf_end);
}

void usys_perf_null(void)
{
	chcore_syscall0(CHCORE_SYS_perf_null);
}

void usys_top(void)
{
	chcore_syscall0(CHCORE_SYS_top);
}

int usys_user_fault_register(int notific_cap, vaddr_t msg_buffer)
{
	return chcore_syscall2(CHCORE_SYS_user_fault_register, notific_cap, msg_buffer);
}

int usys_user_fault_map(u64 client_badge, vaddr_t fault_va, vaddr_t remap_va, bool copy)
{
	return chcore_syscall4(CHCORE_SYS_user_fault_map,
		client_badge, fault_va, remap_va, copy);
}


int usys_map_pmo_with_length(int pmo_cap, vaddr_t addr, u64 perm, size_t length)
{
	return chcore_syscall5(CHCORE_SYS_map_pmo, SELF_CAP,
		pmo_cap, addr, perm, length);
}

int usys_irq_register(int irq)
{
	return chcore_syscall1(CHCORE_SYS_irq_register, irq);
}

int usys_irq_wait(int irq_cap, bool is_block)
{
	return chcore_syscall2(CHCORE_SYS_irq_wait, irq_cap, is_block);
}

int usys_irq_ack(int irq_cap)
{
	return chcore_syscall1(CHCORE_SYS_irq_ack, irq_cap);
}

int usys_create_notifc()
{
	return chcore_syscall0(CHCORE_SYS_create_notifc);
}

int usys_wait(u32 notifc_cap, bool is_block, void *timeout)
{
	return chcore_syscall3(CHCORE_SYS_wait, notifc_cap, is_block,
			       (u64)timeout);
}

int usys_notify(u32 notifc_cap)
{
	int ret;

	do {
		ret = chcore_syscall1(CHCORE_SYS_notify, notifc_cap);

		if (ret == -EAGAIN) {
			// printf("%s retry\n", __func__);
			usys_yield();
		}
	} while (ret == -EAGAIN);
	return ret;
}

/* Only used for recycle process */
int usys_register_recycle_thread(int cap, u64 buffer)
{
	return chcore_syscall2(CHCORE_SYS_register_recycle,
			       cap, buffer);
}

int usys_cap_group_recycle(int cap)
{
	return chcore_syscall1(CHCORE_SYS_cap_group_recycle,
			       cap);
}

/* Get the size of free memory */
u64 usys_get_free_mem_size(void)
{
	return chcore_syscall0(CHCORE_SYS_get_free_mem_size);
}

u64 usys_map_with_pmo(u64 pmo_cap, u64 perm)
{
	return chcore_syscall2(CHCORE_SYS_map_with_pmo, pmo_cap, perm);
}

int usys_unmap_with_addr(u64 addr)
{
	return chcore_syscall1(CHCORE_SYS_unmap_with_addr, addr);
}

int usys_cache_flush(u64 start, u64 size, int op_type)
{
	return chcore_syscall3(CHCORE_SYS_cache_flush, start, size, op_type);
}

int usys_memcpy_and_flush_tlb(u64 src_pa, u64 dst_pa, u64 len, u64 fault_va, u64 vmspace)
{
	return chcore_syscall5(CHCORE_SYS_memcpy_and_flush_tlb, src_pa, dst_pa, len, fault_va, vmspace);
}

u64 usys_get_current_tick(void)
{
	return chcore_syscall0(CHCORE_SYS_get_current_tick);
}

u64 usys_virt_dispatch(u64 syscall_no, u64 param1, u64 param2, u64 param3, u64 param4, u64 param5)
{
	return chcore_syscall6(CHCORE_SYS_virt_dispatch, syscall_no, param1, param2, param3, param4, param5);
}

int usys_whole_ckpt(char *ckpt_name, u64 name_len)
{
	return chcore_syscall2(CHCORE_SYS_whole_ckpt, (u64)ckpt_name, name_len);
}

int usys_whole_ckpt_for_test(char *ckpt_name, u64 name_len, u64 log_level)
{
	return chcore_syscall3(CHCORE_SYS_whole_ckpt_for_test, (u64)ckpt_name, name_len, log_level);
}

int usys_whole_restore(char *ckpt_name, u64 name_len)
{
	return chcore_syscall2(CHCORE_SYS_whole_restore, (u64)ckpt_name, name_len);
}

int usys_register_external_ringbuf(u64 buffer)
{
	return chcore_syscall1(CHCORE_SYS_register_external_ringbuf, buffer);
}

int usys_cfork_prepare(char *pname, u64 pname_len)
{
	return chcore_syscall2(CHCORE_SYS_cfork_prepare, (u64)pname, pname_len);
}

int usys_cfork_ckpt(char *pname, u64 pname_len)
{
	return chcore_syscall2(CHCORE_SYS_cfork_ckpt, (u64)pname, pname_len);
}

int usys_cfork_restore(char *pname, u64 pname_len)
{
	return chcore_syscall2(CHCORE_SYS_cfork_restore, (u64)pname, pname_len);
}

int usys_ckpt_process(char *pname, u64 pname_len)
{
	return chcore_syscall2(CHCORE_SYS_ckpt_process, (u64)pname, pname_len);
}

int usys_restore_process(char *pname, u64 pname_len)
{
	return chcore_syscall2(CHCORE_SYS_restore_process, (u64)pname, pname_len);
}

void usys_ipi_stop_all()
{
	chcore_syscall0(CHCORE_SYS_ipi_stop_all);
}

void usys_ipi_start_all()
{
	chcore_syscall0(CHCORE_SYS_ipi_start_all);
}

void usys_ipi_test_kernel(int cpuid)
{
	chcore_syscall1(CHCORE_SYS_ipi_test_kernel, cpuid);
}

int usys_clock_gettime(sysclockid_t clock, struct systimespec *ts)
{
	return chcore_syscall2(CHCORE_SYS_clock_gettime, clock, (u64)ts);
}

void usys_shutdown(int flag)
{
	chcore_syscall1(CHCORE_SYS_shutdown, flag);
}

int usys_clone_cap_group(u64 args)
{
	return chcore_syscall1(CHCORE_SYS_clone_cap_group, args);
}

u64 usys_track_pf_begin()
{
	return chcore_syscall0(CHCORE_SYS_track_pf_begin);
}

u64 usys_track_pf_end()
{
	return chcore_syscall0(CHCORE_SYS_track_pf_end);
}

int usys_get_poll_remote()
{
	return chcore_syscall0(CHCORE_SYS_get_poll_remote);
}

void usys_set_poll_remote()
{
	chcore_syscall0(CHCORE_SYS_set_poll_remote);
}

void usys_set_excepted_connected_client_num(int expected_num) {
	chcore_syscall1(CHCORE_SYS_set_excepted_connected_client_num, expected_num);
}

void usys_set_dyn_args(u64 hotness, u64 access_interval)
{
    chcore_syscall2(CHCORE_SYS_set_dyn_args, hotness, access_interval);
}

int usys_pcie_control(u64 req_buf) {
	return chcore_syscall1(CHCORE_SYS_pcie_control, req_buf);
}

int usys_get_machine_id()
{
	return (int)chcore_syscall0(CHCORE_SYS_get_machine_id);
}

u32 usys_register_fs_client(u32 target_machine_id, u64 shm_config_ptr)
{
	return chcore_syscall2(CHCORE_SYS_register_fs_client, target_machine_id, shm_config_ptr);
}

u32 usys_register_fs_server(u32 fs_cap)
{
	return chcore_syscall1(CHCORE_SYS_register_fs_server, fs_cap);
}

#ifdef IPC_PERF_ENABLED
void usys_ipc_perf_start(void)
{
	chcore_syscall0(CHCORE_SYS_ipc_perf_start);
}

void usys_ipc_perf_end(void)
{
	chcore_syscall0(CHCORE_SYS_ipc_perf_end);
}
#endif

int usys_mmap_shm(u32 shm_id, void *addr)
{
	return chcore_syscall2(CHCORE_SYS_mmap_shm, shm_id, (u64)addr);
}
