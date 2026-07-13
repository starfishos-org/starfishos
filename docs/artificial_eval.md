# SOSP 2023 Artifact Submission

We thank the artifact evaluators who have volunteered to do one of the toughest jobs out there!

## Requirements

Hardware
- Intel® Optane™ Persistent Memory (or you can use Qemu mode for simulation)
- CPU Core >= 20

Software
- docker: we build the OS within a given docker
- qemu-system-x86: use qemu mode to boot the OS
- ipmitool: for interacting with the real machine (with kernel loaded)
- expect: for interacting with the real machine
- python3: for parsing and drawing
    - requirements: matplotlib, pandas, numpy, seaborn

## Building TreeSLS OS

<!-- > Currently, we provide the already-built kernel images, so you can skip this part. -->

Use `./quick-build.sh` to build everything at first.

### Kernel Parameters

Different tests require different flags in `kernel/sls_config.cmake`. But we will give a `setup.sh` script in each test to **automatically** set these parameters and build the kernel!

The meaning of each flag is given below:

1. Basic configuration
    - SLS_RESTORE: restore from the last checkpoint if set; else start with an empty OS.
    - SLS_EXT_SYNC: enable external synchrony.
    - SLS_HYBRID_MEM: enable `hybrid method` to checkpoint memory pages; else fall back to `CoW method` during runtime.

2. Report details
    - SLS_REPORT_CKPT: report checkpoint information
    - SLS_REPORT_RESTORE: report restore information
    - SLS_REPORT_HYBRID: report information of hybrid method

3. Special tests
    - SLS_SPECIAL_OMIT_PF: omit triggering page fault related to checkpoint
    - SLS_SPECIAL_OMIT_MEMCPY: omit to copy page-faulted pages related to checkpoint
    - SLS_SPECIAL_OMIT_BENCHMARK: omit tracking benchmarks

### User App Parameters

Also, you can selectively choose whether to build each application by setting the `ON` flag in `user/config.cmake`. 

```cmake
chcore_config(CHCORE_DEMOS_REDIS BOOL ON "Build redis?")
chcore_config(CHCORE_DEMOS_MEMCACHED BOOL ON "Build memcached?")
chcore_config(CHCORE_DEMOS_MEMCACHETEST BOOL ON "Build memcache test?")
chcore_config(CHCORE_DEMOS_SQLITE BOOL ON "Build SQLite3?")
chcore_config(CHCORE_DEMOS_LEVELDB BOOL ON "Build LevelDB?")
chcore_config(CHCORE_DEMOS_YCSB BOOL ON "Build YCSB-C?")
chcore_config(CHCORE_DEMOS_PHOENIX BOOL ON "Build Phoenix?")
chcore_config(CHCORE_DEMOS_ROCKSDB BOOL ON "Build RocksDB?")
```

## Knowledge Before Testing!

Please replace `basedir` in `artificial_evaluation/config.exp` and `artificial_evaluation/config.sh`. (*you should modify both*)

We will give scripts of each test (`*.sh` files) in **subdirs** in `artificial_evaluation/`.

### Test Mode

You can use `QEMU` or `IPMI` mode. Switch the mode by setting mode in `artificial_evaluation/config.exp` and `artificial_evaluation/config.sh` (*you should modify both*). We **recommend** you use the `IPMI` mode since QEMU's simulation of NVM is quite different from the real cases.

<!-- ### Run QEMU mode 

To run with the QEMU mode, you can use `./qemu.exp` to -->
<!-- If you like to use other images, please replace that in `./qemu.exp`.  -->

### More information about using the IPMI mode!

To run with the `IPMI` mode, you should:

1. Build the os image (currently, no building) and load the `build/treesls.iso` file (or the provided `images/treesls-*.iso`) to the iDRAC platform.

    ![2-load-treesls](./load-treesls.png)

2. Boot the os with a grub entry:
    ```
    menuentry 'treesls' {
        insmod (hd0,gpt1)/efi/boot/x86_64-efi/multiboot2.mod
        multiboot2 (cd0)/boot/kernel.img
        boot
    }
    ```

    or manually use:

    ```
    grub
    > insmod (hd0,gpt1)/efi/boot/x86_64-efi/multiboot2.mod
    > multiboot2 (cd0)/boot/kernel.img
    > boot
    ```
    To install the `multiboot2.mod`, you should:
    ```
    sudo apt/yum install grub-efi
    sudo cp -r /usr/lib/grub/x86_64-efi /boot/efi/EFI/BOOT/
    ```
    <!-- ![1-boot-treesls](./boot-treesls.png) -->

3. Wait a minute and interact with the os by ipmitool.


## Evaluating the Artifact

In most cases, the workflow of running each test is:
1. Load
    - If you directly use the provided images: 
        - in IPMI mode: load `images/treesls*.iso`, the special iso name is listed in each test.
        - in QEMU mode: use `images/treesls*.iso` to replace `build/treesls.iso`.
    - If you want to build the image:
        - use `setup*.sh` in each subdir to set kernel flags and build the image.
        - in IPMI mode: load the `build/treesls.iso` image to boot it.
        - in QEMU mode: do nothing
2. Run: use `test*.sh` to run the test and the logs are stored in `artificial_evaluation/logs/<mode>/`
3. Parse the logs: use `table*.sh` or `fig*.sh` to parsing the data and generate results (*.jpg or *.csv) in `artificial_evaluation/<subdir>/result/`

There are some problems that might occur during the testing:
1. Building image failed: currently, we build with `./chbuild build`, if any conflict occurs, you can build with `./quick-build.sh` or `./chbuild clean` first.
2. IPMI connection failed: currently, BMC sometimes close unexpectedly, and our retry scripts sometimes still can not handle everything well, so a manual re-test is needed.

**NOTE**: *We recommend you run all tests with `artificial_evaluation/test_base_all.sh` together and run tests with other required setups separately!*

### 0. Functionality

We use QEMU mode to test the functionality, that is, whether our programs can restart with the same working flow as the time it crashes.

You should:

1. use `start.exp` to start the program, we test the ping-pong program by default, we can test whatever you like by replacing `send -- "test_crash_counter.bin & \r"` in the script.
2. during the running of the program, you can use 'CTRL-A + X' to stop the QEMU (crash the program).
3. now you can use `restore.exp` to restart from the latest checkpoint and check the output.


### 1. Checkpoint/Restore Details (Table 2 & 3, Figure 9)

This test reports the checkpoint/restore details as well as other configurations like app size and object count.

#### 1.1 ckpt details

0. run `./setup_ckpt_details.sh` and load `build/treesls.iso` (or load `images/treesls-ckpt.iso`)
1. use `./test_ckpt_details.sh` to run each benchmark with the checkpoint log reported
2. run `./fig9.sh` and `./table4.sh`

#### 1.2 restore details

0. run `./setup_restore_details.sh` and load (or load `images/treesls-restore.iso`)
1. use `./test_restore_details.sh` to run each benchmark with the restore log reported
2. run `./table3.sh`

#### 1.3 object count as well as size

0. run `./setup_ckpt_size_details.sh` or `cp images/treesls-mem-size.iso build/treesls.iso` (no need to load, size calculated in QEMU mode)
1. use `./test_ckpt_size.exp` to calculate the memory size 
2. run `./table2.sh`

### 2. Hybrid memory checkpoint method (Table 4 & Figure 10)

Information in Table 4 is together tested with Test 1 (results are generated by `1-ckpt-restore-details/test-ckpt-details.sh`). You can just run `table4.sh` to get the result.

Figure 10 requires 4 different setups:
1. `+ckpt`: 
    - build with `./setup_plusckpt.sh` or use `images/treesls-plusckpt.iso`
    - run `./test_plusckpt.sh`
2. `+pf`: 
    - build with `./setup_pluspf.sh` or use `images/treesls-pluspf.iso`
    - run `./test_pluspf.sh`
3. `+memcpy`: 
    - build with `./setup_plusmemcpy.sh` or use `images/treesls-plusmemcpy.iso`
    - run `./test_plusmemcpy.sh`
4. `base and hybrid`: base can be tested with any setup (as no checkpoint here), we put it with hybrid setup. 
    - build with `./setup_base_and_hybrid.sh` or use `images/treesls-plusmemcpy.iso`images/treesls-base.iso` 
    - run `./test_base_and_hybrid.sh`. (recommended to run this together with other tests based on the same image by `artificial_evaluation/test_base_all.sh`).

After all, run `fig10.sh`

### 3. External Synchrony Support (Figure 12)

1. build with `./setup_base.sh` (or use `images/treesls-base.iso`) and run `./test_base.sh`. (recommended to run by artificial_evaluation/test_base_all.sh)
2. build with `./setup_ext_sync.sh` (or use  `images/treesls-ext.iso`) and run `./test_ext_sync.sh`.

After all, run `./fig12.sh`.

> Note: The following tests (4, 5, 6) can both run with the base image (use `./setup.sh` in any subdir or use `images/treesls-base.iso`). We recommend you run with artificial_evaluation/test_base_all.sh together!

### 4. Memcached (Figure 11)

1. run `./test_memcached.sh`
2. run `./fig11.sh`

### 5. Redis-YCSB (Figure 13)

1. run treesls tests: enter `5-redis-ycsb` and run `./test_ycsb.sh`
2. run linux tests 
    - run on the same machine where treesls is loaded to
    - enter `5-linux-redis-ycsb`
    - update the git submodule and use `./setup.sh` to prepare everything
    - run `./test_ycsb.sh`
    - copy the subdirs in `5-linux-redis-ycsb/logs/` to `artificial_evaluation/logs/<mode>/ycsb/`
3. run `./fig13.sh`

### 6. RocksDB-Prefix_dist (Figure 14)

1. and run `./test_rocksdb.sh`
2. run Rocksdb test provided by Aurora (https://github.com/rcslab/aurora-bench/tree/master), scripts are given in `6-aurora-rocksdb/test_rockdb.sh`
3. copy logs of Aurora (by default, in `/aurora-data/`) to `artificial_evaluation/logs/<mode>/rocksdb/`
3. run `./fig14.sh`

## Estimated Resources

Space are consumed within the microkernel.

| Test                                  | Estimated Time         | Details |
| -------------------------------------- | ---------------------- | ---------- |
| 1/test_ckpt_details.sh                 | ~24 min   | ~3 min per workload, 8 workloads      |           |            |
| 1/test_ckpt_size.sh                    | ~18 min   | ~3 min per workload, 6 workloads        |          |            |
| 1/test_restore_details.sh            |    ~21 min    | ~3 min per workload, 7 workloads     |                                    
| 2/test_*.sh                            | ~1 hour (~12 min * 5 configurations)      |  ~3 minutes per workload, 4 workloads, 5 configurations |        |
| 3/test_base.sh + test_ext_syn.sh       | ~9 min (ext-sync) + 12 min (base)        | ~3 min per workload, 3 configurations (ext-sync) + 4 configurations (base) |         |
| 4/test_memcached.sh     | ~15 min          | ~3 min per test, 5 configurations        |            |
| 5/test_ycsb.sh          | ~32 min              | ~4 min per test, 4 * 2 configurations        | 
| 6/test_rocksdb.sh          | ~6 min              | ~3 min per test, 2 configurations        | 

## Common Q&A
Q. Changing the CPU number

A. Currenly we hard code the CPU number. To change it, you can:
- Open the file `kernel/arch/x86_64/boot/CMakeLists.txt`. Change line 36 `-smp XXX` with a new value.
- Open the file `/kernel/include/arch/x86_64/plat/intel/machine.h`. Change line 4 `#define PLAT_CPU_NUM XXX` to half the value used above. For example, if using `-smp 20`, change it to `#define PLAT_CPU_NUM 10`. (we use half of the cores to use a single NUMA on the server).

Q. QEMU bug:

```
[INFO] General Protection Fault
[INFO] Faulting Address: 0x0
[INFO] Current thread 0
[INFO] Trap from 0xffffffffc011338b EC 0 Trap No. 13
[INFO] DS 0x0, CS 0x10, RSP 0xffffffffc0109030, SS 0x18
[INFO] rax: 0x706b0, rdx: 0x80010031, rdi: 0xffffffffca14aa80
[INFO] rcx: 0x65
```

A. We have also encountered this bug on Ubuntu 22.04. The issue stems from QEMU having problems with emulating the CPU's PCID feature support on this particular version, thus leading to a General Protection Fault bug.
You can try running it on a machine with a different operating system installed. We have successfully boot it on some machines with Debian and Fedora.