# 3. Collaborative system services

Starfish runs cooperating service instances rather than one fully shared service process. Each instance keeps active state local; limited coordination metadata provides a global process, filesystem, and device view.

## Service layout

| Service | Role | Main code |
| --- | --- | --- |
| Process manager | starts services/apps and returns service capabilities | `procmgr/`, `procmgr/srvmgr.c` |
| Filesystem base | POSIX filesystem IPC, vnodes, page cache | `fs_base/` |
| tmpfs | in-memory filesystem and recovery | `tmpfs/` |
| Filesystem manager | mount/service coordination | `fsm/` |
| POSIX shm, network, shell | user-facing auxiliary services | `posix_shm/`, `lwip/`, `chcore_shell/` |

The build composition is [user/system-servers/CMakeLists.txt](../user/system-servers/CMakeLists.txt) plus feature switches in `.config`.

## Global access and recovery

Applications obtain service capabilities through the process manager and use ordinary IPC. Therefore a filesystem operation may reach an instance on another machine without changing the POSIX call site. Dispatch is centered in `fs_base/fs_wrapper.c` and `fs_base/fs_wrapper_ops.c`; [fs_page_fault.c](../user/system-servers/fs_base/fs_page_fault.c) supports client mappings.

[procmgr.c](../user/system-servers/procmgr/procmgr.c) boots default services/apps, while [srvmgr.c](../user/system-servers/procmgr/srvmgr.c) starts secondary instances.

For tmpfs recovery:

- [tmpfs/plog.c](../user/system-servers/tmpfs/plog.c): persistent-log append, checkpoint, remote map, and replay;
- [tmpfs/tmpfs.c](../user/system-servers/tmpfs/tmpfs.c): initialization and `recover_tmpfs` (attach CXL checkpoint, replay log tail, re-register as IPC server);
- [tmpfs/tmpfs_ops.c](../user/system-servers/tmpfs/tmpfs_ops.c): namespace operations and log integration.

The end-to-end recovery test kills machine 0, runs `tmpfs.srv --recover 0` on machine 1, then reopens LevelDB. Run `./artifact-evaluation/7-recover-fs/run.sh` after preparation. This repairs the service; LevelDB's durable recovery remains application-owned.

