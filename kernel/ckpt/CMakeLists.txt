target_sources(${kernel_target} 
	PRIVATE ckpt.c
            restore.c
			ckpt_ipi.c
			ckpt_ws.c
			ckpt_object_pool.c
			ckpt_page.c
			ckpt_objects/cap_group.c
			ckpt_objects/connection.c
			ckpt_objects/irq.c
			ckpt_objects/notification.c
			ckpt_objects/pmobject.c
			ckpt_objects/thread.c
			ckpt_objects/vmspace.c
			external_sync.c
			hybird_mem.c
			hot_pages_tracker.c)
