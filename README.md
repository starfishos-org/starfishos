# StarfishOS: a State-Partitioned Microkernel for CXL Pods

[![Platform x86_64](https://img.shields.io/badge/platform-x86__64-4C6EF5?style=flat-square)](#hardware-and-software-requirements)
[![Build Docker](https://img.shields.io/badge/build-Docker-2496ED?style=flat-square&logo=docker&logoColor=white)](https://hub.docker.com/r/promisivia/treesls_chcore_builder)
[![License Mulan PSL v1](https://img.shields.io/badge/license-Mulan%20PSL%20v1-6f42c1?style=flat-square)](LICENSE)

Starfish is an experimental OS-level single-system image (SSI) built on the ChCore microkernel. It lets an unmodified shared-memory application use CPUs, DRAM, and CXL-attached shared memory across a pod of machines. Its central design principle is **state partitioning**: keep hot, private state local to each machine and keep only small, recoverable coordination state in shared CXL memory.

> This is a research prototype for the x86_64/QEMU environment below, not a general-purpose production OS.

## Hardware and software requirements

### Hardware
- CPU: 4 × Intel Xeon Gold 6418H, 24 physical cores each (96 CPUs total).
- DRAM: 4 × 64 GiB = 256 GiB.
- CXL: 128 GiB CXL 1.1 on the evaluation machine; 64 GiB is the minimum for the real-CXL setup.
- NUMA: 6 nodes; nodes 0–3 have CPUs, nodes 4–5 are memory-only.

### Software
- Architecture: x86_64, other architectures are not supported yet.
- Operating System: Ubuntu 22.04/Linux 5.15, (*Note: Do not use Linux 6.1–6.4; our OS cannot boot on affected Linux versions because PCID is disabled due to the `INVLPG` issue described in [Phoronix's report](https://www.phoronix.com/news/Intel-Disable-PCID-ADL-RPL).*)
- QEMU: 6.2 with KVM enabled, other QEMU versions are unsupported yet as they do not have the feature of configuring ivshmem size that our artifact requires.
- Required tools: `numactl`, `tmux`, Python 3, GNU `make`, and Docker or the local `chbuild` toolchain.

### Host dependency installation

On Ubuntu/Debian, install qemu-6.2 and required host-side tools with:

```bash
bash artifact-evaluation/install-host-deps.sh
```

The script enables Docker and adds the invoking user to the `docker` and `kvm`
groups; log out and back in before using Docker or `/dev/kvm` without `sudo`.
It also installs plotting packages (`matplotlib`, `numpy`, `pandas`), downloads
the official QEMU 6.2.0 source package, builds the x86_64/KVM target and
ivshmem server, then installs `/usr/local/bin/qemu-6.2-system-x86_64` and
`/usr/local/qemu-6.2/bin/ivshmem-server`.

### Docker image

We provide a Docker image for building the artifact. To pull the image, run:

```bash
docker pull promisivia/treesls_chcore_builder:v2.3
```

To build the image from scratch, run:

```bash
docker build -t promisivia/treesls_chcore_builder:v2.3 .
```

## Get Started

The default build uses the Docker builder image configured in [chbuild](chbuild).

```bash
git submodule update --init --recursive   # required: libraries, demos, and Linux baselines
bash artifact-evaluation/install-host-deps.sh   # once per host; then re-login
docker pull promisivia/treesls_chcore_builder:v2.3
make prepare        # AE prepare (datasets + NUMA/CXL/hostfs/CXLFS + ivshmem) + first build
make build          # incremental build
make r4             # boot a cluster of 4 QEMU/KVM machines
```

All configured submodules use public HTTPS URLs under
`https://github.com/starfishos-org`. Initialize them before `docker build` as
well as before the normal build; Docker copies the populated `user/` tree from
the host build context. Demo entries use the `starfishos-test` branch for
`git submodule update --remote`; normal checkouts remain pinned by the
superproject's recorded commit.

Each demo repository retains its pre-StarfishOS history and publishes the
`starfishos-upstream-base` tag at the comparison baseline. For example:

```bash
git -C user/demos/redis-6.0.8 diff starfishos-upstream-base..HEAD
```

The annotated tag and the repository homepage record the corresponding
upstream URL and version or commit.

`make prepare` runs `artifact-evaluation/prepare.sh ensure` (including the eight
NUMA `/dev/shm/numa*.?-$USER` backing files and the ivshmem doorbell server)
before `scripts/quick-build.sh`. Skipping it leaves `make r4` unable to boot
when `USE_DEV_AS_DRAM=1`.

The launcher defaults to 12 vCPUs per guest and a 64 GiB CXL backing file. Set `machine_num`, `cpu_num`, `dram_size`, and `cxl_size` in the repository-root [`chcore.ini`](chcore.ini) to change the persistent cluster configuration. Environment variables such as `MACHINE_NUM`, `CPU_NUM`, and `CXL_SIZE` override those values for a single launch. Per-NUMA backing-file sizes remain configured in [`dsm-scripts/numa_sizes.conf`](dsm-scripts/numa_sizes.conf).


## Artifact evaluation

Follow the **From a fresh clone** section in
[artifact-evaluation/README.md](artifact-evaluation/README.md), then:

```bash
python3 artifact-evaluation/run_all.py    # default: validated ready experiments
```

Each subdirectory documents its inputs, overrides, outputs, and plot regeneration.

## Repository layout

```text
artifact-evaluation/    reproducible runners and plotting scripts
build/                  build system and toolchain, including kernel images
docs/                   paper-structured design and implementation guide
dsm-scripts/            scripts for setting up and testing the distributed StarfishOS
scripts/                scripts for boosting/building/gdb/elf-parsing/etc.
kernel/                 kernel-space source code
user/
|--demos/               paper workloads and ported applications
|--libraries/           shared libraries
|--musl-1.1.24/         musl libc 1.1.24 source code
|--sample-apps/         sample applications
|--scripts/             scripts that is called in the OS
|--sys-include/         header files for the system services
|--system-servers/      collaborative process, filesystem, and device services
Dockerfile              Dockerfile for building the artifact
chcore.ini              configuration file for the artifact
chbuild,quick-build.sh  script for building the artifact
```

## Documentation

The documentation follows the paper:

1. [Design overview](docs/01-design-overview.md) — execution, hardware, state partitioning, and failure model.
2. [Kernel-space modules](docs/02-kernel-space-modules.md) — IPC, scheduling, notification, memory, and recovery.
3. [Collaborative system services](docs/03-collaborative-system-services.md) — global namespaces and filesystem recovery.
4. [Cross-machine applications](docs/04-cross-machine-applications.md) — migration and application state placement.
5. [Implementation map](docs/05-implementation-map.md) — direct links from paper mechanisms to source.

See [docs/README.md](docs/README.md) for the full index. If checked out alongside this repository, `../p3os-paper/` contains the Starfish manuscript and its source figures.


## Publication

StarfishOS: Revisiting Single System Image on CXL with State-Partitioned Microkernel. 
Fangnuo Wu, Jingsheng Yan, Mingkai Dong, Wenjun Cai, Jingwei Xu, Tong Xin, Haibo Chen.
The 32nd Symposium on Operating Systems Principles (SOSP’26), Prague, Czechia, September 29 - October 2, 2026.


## License

The Starfish artifact is licensed under the Mulan PSL v1 License. See [LICENSE](LICENSE) for details.
