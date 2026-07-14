# Artifact evaluation

## Hardware and software requirements

### Paper's Default Testbed

Hardware:
- CPU: 4 × Intel Xeon Gold 6418H, 24 physical cores each (96 CPUs total).
- DRAM: 4 × 64 GiB = 256 GiB.
- CXL: 128 GiB CXL 1.1 on the evaluation machine; 64 GiB is the minimum for the real-CXL setup.
- NUMA: 6 nodes; nodes 0–3 have CPUs, nodes 4–5 are memory-only.

Software:
- x86_64, Ubuntu 22.04/Linux 5.15
- QEMU 6.2 with KVM enabled
- Required tools: `numactl`, `tmux`, Python 3, GNU `make`, and Docker or the local `chbuild` toolchain.

Host dependency installation:

On Ubuntu/Debian, install qemu-6.2 and required host-side tools with:

```bash
bash artifact-evaluation/install-host-deps.sh
```

The script enables Docker and adds the invoking user to the `docker` group; log out and back in before using Docker without `sudo`. It downloads the official QEMU 6.2.0 source package, builds the x86_64/KVM target and ivshmem server, then installs `/usr/local/bin/qemu-6.2-system-x86_64` and `/usr/local/qemu-6.2/bin/ivshmem-server`.

### Hardware/Software Requirement

Hardware Requirement:
- Memory requirement: CXL >= 64GiB; DRAM >= 144GiB (8*16GiB DRAM + 16GiB hostfs)
- CPU requirement: >= 96 CPUs

*Note: you can change the CPU cores or memory size by changing the `CPU_NUM` and `DRAM_SIZE` variables in the `dsm-scripts/numa_sizes.conf` file, but some tests may not work properly.*

Software Requirement:
- Architecture: x86_64, other architectures are not supported yet.
- Operating System: Ubuntu 22.04/Linux 5.15, (*Note: Do not use Linux 6.1–6.4; our OS cannot boot on affected Linux versions because PCID is disabled due to the `INVLPG` issue described in [Phoronix's report](https://www.phoronix.com/news/Intel-Disable-PCID-ADL-RPL).*)
- QEMU: 6.2 with KVM enabled, other QEMU versions are unsupported yet as they do not have the feature of configuring ivshmem size that our artifact requires.
- Required tools: `numactl`, `tmux`, `Python 3` (`matplotlib` is required for plotting), `GNU make`, and `Docker`.

Download the docker image from Docker Hub for building the artifact:

```bash
docker pull promisivia/treesls_chcore_builder:v2.3
```

(Optional) If you want to build the image from scratch instead, run this command from the repository root:

```bash
docker build -t promisivia/treesls_chcore_builder:v2.3 .
```

---

## One-click（唯一入口）

```bash
python3 artifact-evaluation/run_all.py
# 等价：
./artifact-evaluation/run_all.py
./artifact-evaluation/run-all.sh          # 薄封装，转调 run_all.py
```

clone 之后一般**只需要这一条**。首次运行会自动：

| 步骤 | 行为 | 何时执行 |
| --- | --- | --- |
| `prepare.sh` | CXL / 8×NUMA / hostfs / ivshmem doorbell | 每次（已存在则跳过重建） |
| first-time OS build | `scripts/quick-build.sh`（= `make prepare` 的编译步） | **仅当**缺少 `.config` 或 `build/kernel.img` |
| 论文实验 | 各目录 `run.sh` 测完画图，汇总到 `out/<ts>/figures/` | 每次 |
| TODO 占位 | 未实现的论文图写 `TODO.md` / `FIGURES.md` | 每次 |

```bash
python3 artifact-evaluation/run_all.py --dry-run          # 只看计划
python3 artifact-evaluation/run_all.py --list             # 列出实验
python3 artifact-evaluation/run_all.py --prepare-only     # 只做环境准备
python3 artifact-evaluation/run_all.py --prepare-only --prepare-mode recreate
python3 artifact-evaluation/run_all.py --build-only       # 只做首次/强制编译
python3 artifact-evaluation/run_all.py --experiments-only ready
python3 artifact-evaluation/run_all.py --gather-only      # 只汇总已有图
python3 artifact-evaluation/run_all.py --force-base-build # 强制重编译后全跑
```

### 阶段参数

| 参数 | 作用 |
| --- | --- |
| `--prepare-only` | 只跑 `prepare.sh` 后退出 |
| `--build-only` | 只做 OS 首次/强制编译后退出 |
| `--gather-only` | 只从各实验目录收图，不开 QEMU |
| `--experiments-only` | 跳过 prepare + build，只跑实验 |
| `--prepare` / `--no-prepare` | 开关 prepare（默认开） |
| `--prepare-mode ensure\|recreate` | `ensure` 幂等（默认）；`recreate` 重建 backing files |
| `--build` / `--no-build` | 开关首次 OS 编译检查（默认开） |
| `--force-base-build` | 强制 `quick-build.sh` |
| `--run` / `--no-run` | 开关实验 QEMU |
| `--gather` / `--no-gather` | 开关收图 |
| `--budget SECS` | 覆盖所有 ready 实验的超时 |
| `--out DIR` | 指定输出目录 |

### 论文图状态

| 实验 | 论文图 | 状态 |
| --- | --- | --- |
| `1-ipc-cdf` | IPC CDF + breakdown | ready |
| `3-memory-allocator` | `fig00-allocator-all` | ready |
| `4-state-partition` | `state_partition` | ready |
| `6-auto-scale` | `auto-scale-matrix` / `db1000` / `gemini-chcore` | **TODO** |
| `8-process-migration` | `process-migration-data-*` | **TODO** |
| `9-resource-util` | `real.eps` | **TODO** |
| `7-recover-fs` | `recovery-performance-single` | ready |

缺图清单：[`TODO-FIGURES.md`](TODO-FIGURES.md)。

### 可选模式

```bash
python3 artifact-evaluation/run_all.py ready     # 只跑已实现的 4 个
python3 artifact-evaluation/run_all.py all       # 论文 + extras
python3 artifact-evaluation/run_all.py ipc-cdf   # 指定子集
```

| 参数 / Env | Meaning |
| --- | --- |
| `--dry-run` / `DRY_RUN=1` | 只打印计划 |
| `--prepare-only` | 只 prepare |
| `--prepare-mode` / `PREPARE_MODE` | `ensure`（默认）或 `recreate` |
| `--no-prepare` / `SKIP_PREPARE=1` | 跳过 prepare |
| `--build-only` | 只 OS build |
| `--no-build` / `SKIP_BASE_BUILD=1` | 跳过 OS build 检查 |
| `--force-base-build` / `FORCE_BASE_BUILD=1` | 强制 `quick-build.sh` |
| `--experiments-only` | 跳过 prepare+build，只跑实验 |
| `--gather-only` | 只收图 |
| `--budget SECS` | 全局实验超时 |
| `--out DIR` | 输出目录 |
| `BUDGET_<NAME>=secs` | 单实验超时覆盖 |

实现：编排在 [`run_all.py`](run_all.py)；各实验的 QEMU 流程仍在对应目录的 `run.sh` / `plot.py`。

---

## 底层准备脚本（通常不必单独跑）

```bash
./artifact-evaluation/prepare.sh
./artifact-evaluation/prepare.sh recreate
```

## 单实验入口

```bash
./artifact-evaluation/1-ipc-cdf/run.sh
./artifact-evaluation/2-sched-notify-latency/run.sh
./artifact-evaluation/3-memory-allocator/run.sh
./artifact-evaluation/4-state-partition/run.sh
./artifact-evaluation/5-dbx1000-cross-warehouse/run.sh
./artifact-evaluation/7-recover-fs/run.sh
# TODO stubs: 6-auto-scale / 8-process-migration / 9-resource-util
```

Each subdirectory has a README describing its workload, configuration matrix,
environment overrides, raw logs, and figure-regeneration command. Generated
files are written under `out/<timestamp>/` for the sweep experiments and
directly in the evaluation directory for the self-contained ones. The
filesystem-recovery evaluation keeps only its latest raw logs in its fixed
`logs/` directory.
