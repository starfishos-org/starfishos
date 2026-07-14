# Starfish documentation

This guide follows the paper's order. Each chapter explains the mechanism first and then identifies the code that implements it.

| Paper chapter | Guide | Primary code |
| --- | --- | --- |
| Design overview | [01 — Design overview](01-design-overview.md) | `kernel/arch/`, `kernel/dsm/`, `dsm-scripts/` |
| Kernel-space modules | [02 — Kernel modules](02-kernel-space-modules.md) | `kernel/ipc/`, `kernel/sched/`, `kernel/mm/`, `kernel/irq/` |
| Collaborative system services | [03 — Services](03-collaborative-system-services.md) | `user/system-servers/` |
| Cross-machine applications | [04 — Applications](04-cross-machine-applications.md) | `kernel/dsm/`, `user/demos/` |
| Implementation cross-reference | [05 — Implementation map](05-implementation-map.md) | whole tree |

Read the [repository README](../README.md) for build, Docker, and artifact quick-start instructions. Older TreeSLS / CXL investigation notes were removed when this guide was introduced; use the chapters below as the current architecture reference.

## Terms

- **Machine**: one QEMU/KVM guest in the emulated pod, or one physical node in the intended deployment.
- **CXL state**: shared coordination state in the CXL/ivshmem-backed region.
- **Local state**: hot, per-machine state in private DRAM.
- **DSM object**: the distributed representation of a kernel object, which determines placement and cross-machine behavior.
- **Partial failure**: loss of one machine while surviving machines continue; it is not transparent application restart.

