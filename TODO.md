# TODO

1. error
user/system-servers/fs_base/fs_page_fault.c:76
must run once to fill fmap_area_mappings

2. opt
kernel/ipc/connection.c:374
flush tlb only when necessary

3. remote-ipc
kernel/ipc/connection.c:377
kernel/ipc/connection.c:435
change remote ipc logic

client->server->migrate to target machine->do the task->migrate to client machine->client


写文档说明IPC和系统服务的流程

机器A和机器B，机器B上的线程b想要访问机器A上的文件系统
1. 机器A首先访问一次文件系统，生成一个tmpfs server thread

回收CXL上上挂了的线程

Phoenix 支持 线程挂掉后迁移任务
IPC timeout机制

PCIe MSI 发消息