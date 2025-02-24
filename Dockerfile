FROM ubuntu:20.04

ENV DEBIAN_FRONTEND=noninteractive 
# install required packages
RUN apt-get update && \
    apt-get install -y cmake \ 
    clang \
    automake \
    autoconf \ 
    autotools-dev \
    libtool \
    python \
    g++ \
    git \
    wget \
    pkg-config \
    cpio \
    grub2 \
    xorriso \
    flex \
    bison \
    bc

# install elf tools
COPY ./scripts/read_procmgr_elf_tool /home/read_procmgr_elf_tool
WORKDIR /home/read_procmgr_elf_tool
RUN gcc elf.c main.c -o read_procmgr_elf_tool && \
    mkdir -p /usr/bin/ && \
    cp read_procmgr_elf_tool /usr/bin/

# copy user dir 
COPY ./user /chos/user

# install musl-cross-make
WORKDIR /home
RUN git clone https://ghproxy.com//https://github.com/richfelker/musl-cross-make.git
WORKDIR /home/musl-cross-make
COPY ./scripts/musl-cross-make/config.mak .
RUN  sed -i 's|MUSL_SRCDIR = $(REL_TOP)/musl-$(MUSL_VER)|MUSL_SRCDIR = /chos/user/musl-1.1.24|' Makefile && \
    export C_INCLUDE_PATH="/chos/user/sys-include" && \
    make -j12 && \
    make install -j12

# install libevent (for memcached)
WORKDIR /home
RUN git clone https://ghproxy.com//https://github.com/libevent/libevent.git && \
    cd libevent && \
    ./autogen.sh && \
    ./configure --disable-openssl --disable-debug-mode --disable-samples -prefix=/usr/libevent && \
    make CC=/chos/user/musl-1.1.24/build/bin/musl-gcc -s && \
    make install

# install hiredis (for YCSB-C) 
WORKDIR /chos/user/demos/YCSB-C/redis/hiredis
RUN make CC=/chos/user/musl-1.1.24/build/bin/musl-gcc -s && \
    make install 
