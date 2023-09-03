# TreeSLS: A Whole-system Persistent Microkernel with Tree-structured State Checkpoint on NVM

TreeSLS is a microkernel with transparent whole-system persistent support by quickly checkpointing every state in the system.

## Publication

Fangnuo Wu, Mingkai Dong, Gequan Mo, Haibo Chen. TreeSLS: A Whole-system Persistent Microkernel with Tree-structured State Checkpoint on NVM. The 29th ACM Symposium on Operating Systems Principles (SOSP 2023).

## Getting Started

First, clone the repo and checkout the ae branch:

```
git clone https://ipads.se.sjtu.edu.cn:1312/opensource/treesls.git
cd treesls
```

To build the OS, you can use:

``` 
./quick-build.sh
```
or use 
```shell
./chbuild defconfig x86_64 # x86_64 can be changed to other platforms, but treesls is now only implemented on x86_64
./chbuild build
``` 

### Run in QEMU

```shell
./qemu.exp # with a clean NVM backend
```

or

```shell
./build/simulate.sh # with the old NVM backend
```

### Docker

By default, we provide a pre-built Docker image, you can simply use the build command and it will be automatically downloaded. 

If you want to build this image from scratch, you can use the following command to build from the provided dockerfile.

```shell
docker build -t <image_name> .
```

To use the newly built container, you can modify the Docker image name in the `chbuild` file (specifically, line 218 in the `_docker_run()` function) to the image you have built.

## Artificial Evaluation

Please refer to [artificial_eval.md](./artificial_eval.md)

## File Tree

```
|- artificial_evaluation    scripts for artificial evaluation
|- build
    |- treesls.iso          built os image
    |- simulate.sh          qemu simulation script
|- images                   provided os images with different setups
|- kernel                   
    |- ckpt                 treesls checkpoint code
    |- others               other kernel modules
    |- sls_config.cmake     kernel flags related to treesls
|- scripts                  building scripts
|- tests                    some tests
|- user
    |- demos                ported real-world applications
    |- musl-1.1.24          libc for treesls
    |- sample-apps          some small applications
    |- sys-include          headers for userspace system servers
    |- system-servers       userspace system servers
    |- config.cmake         user applications flags
```

### TreeSLS's Implementation

Please refer to [TreeSLS.md](./docs/TreeSLS.md)

## LICENSE

LICENSE of ported applications are given in subdirs.
