Each machine has a filesystem instance：/home/wfn/chcore-cxl/user/system-servers/tmpfs

For ach fs inst: 
1. prepare a p-log on cxl (should be able to find after crash)
2. （文件系统的其他数据应该放在DRAM就行？）
2. use Ananke to record the p-log

After fs inst and machine 0 crash.
On machine 1, start a new fs inst and apply the p-log.
- start a new fs inst: should add a feat: to register fs instance to fsm

reconnect IPC to this fs inst. now cross-machine IPC/old IPC use (?find it, a dq_queue)

测试验证，基于现在的IPC测试：'/home/wfn/chcore-cxl/user/system-servers/polling/polling_client_test.c' 


写一个新的，让machine1的polling程序在调用fs write的一半的时候crash，然后启动一个新的fs inst在machine0，看应用程序能不能透明连上新的fs inst，还能继续运行。