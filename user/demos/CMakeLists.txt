
# demos
set(MUSL_CC ${CHCORE_MUSL_LIBC_INSTALL_DIR}/bin/musl-gcc)
set(MUSL_CXX ${CHCORE_MUSL_CROSS_MAKE_INSTALL_DIR}/bin/x86_64-linux-musl-g++)
set(CMAKE_CXX_COMPILER ${MUSL_CXX})
set(CMAKE_C_COMPILER ${MUSL_CC})

set(_demos_clean_targets)
chcore_get_nproc(_nproc)

set(OS CHCORE)

if(CHCORE_DEMOS_LIGRA)
  add_subdirectory(${build_demos_dir}/ligra)
endif()

if(CHCORE_DEMOS_REDIS)
  add_custom_target(
    redis-clean
    WORKING_DIRECTORY ${build_demos_dir}/redis-6.0.8
    COMMAND make clean
)
list(APPEND _demos_clean_targets redis-clean)
  add_custom_target(
    redis ALL
    WORKING_DIRECTORY ${build_demos_dir}/redis-6.0.8
    COMMAND make CC=${MUSL_CC}
    COMMAND rsync -a src/redis-cli ${build_ramdisk_dir}/redis-cli
    COMMAND rsync -a src/redis-server ${build_ramdisk_dir}/redis-server
    COMMAND rsync -a src/redis-benchmark ${build_ramdisk_dir}/redis-benchmark
    COMMAND rsync -a redis.conf ${build_ramdisk_dir}/redis.conf
    DEPENDS)
  add_dependencies(ramdisk redis)
endif()

if(CHCORE_DEMOS_YCSB)
  add_custom_target(
    ycsb-clean
    WORKING_DIRECTORY ${build_demos_dir}/YCSB-C
    COMMAND make clean
)
list(APPEND _demos_clean_targets ycsb-clean)
  add_custom_target(
    ycsb ALL
    COMMAND export LD_LIBRARY_PATH=$LD_LIBRARY_PATH:/usr/local/lib # to find libhiredis
    # COMMAND export CC=${MUSL_CC}
    # COMMAND export CXX=${MUSL_CC}
    WORKING_DIRECTORY ${build_demos_dir}/YCSB-C/core
    COMMAND make CC=${MUSL_CC} CXX=${MUSL_CXX} -s
    WORKING_DIRECTORY ${build_demos_dir}/YCSB-C/redis
    COMMAND make CC=${MUSL_CC} CXX=${MUSL_CXX} -s
    WORKING_DIRECTORY ${build_demos_dir}/YCSB-C/db
    COMMAND make CC=${MUSL_CC} CXX=${MUSL_CXX} -s
    WORKING_DIRECTORY ${build_demos_dir}/YCSB-C
    COMMAND make CC=${MUSL_CC} CXX=${MUSL_CXX} ycsb -s
)

  add_custom_command(
    TARGET ycsb
    POST_BUILD
    COMMAND rsync -a /usr/local/lib/libhiredis.so.0.13 . # already builded in docker
    COMMAND rsync -a ${build_demos_dir}/YCSB-C/ycsbc .
    COMMAND rsync -a ${build_demos_dir}/YCSB-C/workloads/*.spec .
    WORKING_DIRECTORY ${build_ramdisk_dir}
    DEPENDS)
  add_dependencies(ramdisk ycsb)
endif()

# libevent is already installed in docker images
if(CHCORE_DEMOS_LIBEVENT)
  add_custom_target(
    libevent ALL
    WORKING_DIRECTORY ${build_demos_dir}/libevent
    COMMAND ./autogen.sh
    COMMAND ./configure --disable-openssl --disable-debug-mode --disable-samples -prefix=/usr/libevent
    COMMAND CC=${MUSL_CC} -s &> /dev/null
    COMMAND make install
  )
  add_dependencies(ramdisk libevent)
endif()

if(CHCORE_DEMOS_MEMCACHED)
  set(MEMCACHED_BUILD_DIR ${build_demos_dir}/memcached)

  add_custom_target(memcached ALL
    WORKING_DIRECTORY ${MEMCACHED_BUILD_DIR}
    COMMAND make CC=${MUSL_CC} 
    COMMENT "Building memcached"
  )

  add_custom_target(
    memcached-copy-data
    WORKING_DIRECTORY ${MEMCACHED_BUILD_DIR}
    COMMAND rsync -a memcached ${build_ramdisk_dir}/memcached
    DEPENDS memcached
    COMMENT "Copying memcached to ramdisk"
  )

  add_dependencies(ramdisk memcached-copy-data)

  add_custom_target(memcached-clean
    WORKING_DIRECTORY ${MEMCACHED_BUILD_DIR}
    COMMAND make maintainer-clean-local
    COMMENT "Cleaning memcached"
  )

  list(APPEND _demos_clean_targets memcached-clean)
  add_dependencies(ramdisk memcached)
endif()

if(CHCORE_DEMOS_MEMCACHETEST)
  set(MEMCACHETEST_BUILD_DIR ${build_demos_dir}/memcachetest)

  # 编译阶段
  add_custom_target(memcachetest ALL
    WORKING_DIRECTORY ${MEMCACHETEST_BUILD_DIR}
    COMMAND make CC=${MUSL_CC}
    COMMENT "Building memcachetest"
  )

  # 拷贝到 ramdisk
  add_custom_target(
    memcachetest-copy-data
    WORKING_DIRECTORY ${MEMCACHETEST_BUILD_DIR}
    COMMAND rsync -a ${MEMCACHETEST_BUILD_DIR}/memcachetest ${build_ramdisk_dir}/
    DEPENDS memcachetest
    COMMENT "Copying memcachetest to ramdisk"
  )

  add_dependencies(ramdisk memcachetest-copy-data)

  # 清理目标
  add_custom_target(memcachetest-clean
    WORKING_DIRECTORY ${MEMCACHETEST_BUILD_DIR}
    COMMAND make distclean
    COMMAND ${CMAKE_COMMAND} -E remove -f ${MEMCACHETEST_CONFIG_DONE_FILE}
    COMMENT "Cleaning memcachetest"
  )

  list(APPEND _demos_clean_targets memcachetest-clean)
  add_dependencies(ramdisk memcachetest)
endif()

if(CHCORE_DEMOS_SQLITE)
  add_custom_target(
    sqlite-clean
    WORKING_DIRECTORY ${build_demos_dir}/sqlite
    COMMAND make clean
)
list(APPEND _demos_clean_targets sqlite-clean)
  add_custom_target(
    sqlite ALL
    WORKING_DIRECTORY ${build_demos_dir}/sqlite
    COMMAND make MALLOC=libc CC=${MUSL_CC} -j$(_nproc) -s
    COMMAND rsync -a ${build_demos_dir}/sqlite/*.bin ${build_ramdisk_dir}/
    DEPENDS)
endif()

if(CHCORE_DEMOS_LEVELDB)
  set(LEVELDB_DIR ${build_demos_dir}/leveldb-1.23)
  add_subdirectory(${LEVELDB_DIR})
endif()

if(CHCORE_DEMOS_ROCKSDB)

  file(MAKE_DIRECTORY ${build_demos_dir}/aurora-rocksdb/third-party/gflags-2.2.2/build)
  add_custom_target(
    gflags-clean
    COMMAND rm -rf ${build_demos_dir}/aurora-rocksdb/third-party/gflags-2.2.2/build
    )
    list(APPEND _demos_clean_targets gflags-clean)
  add_custom_target(
    gflags ALL
    WORKING_DIRECTORY ${build_demos_dir}/aurora-rocksdb/third-party/gflags-2.2.2/build
    COMMAND cmake -j$(_nproc) -DCMAKE_BUILD_TYPE=Release
            -DCMAKE_CXX_COMPILER=${MUSL_CXX}
            -DCMAKE_CXX_FLAGS=-fPIC
            -DCMAKE_CROSSCOMPILING=1
            -DBUILD_SHARED_LIBS=ON
            # -DBUILD_SHARED_LIBS=OFF
            -DBUILD_TESTING=OFF
            -S ..
            && cmake --build . -j$(_nproc)
    # COMMAND rsync -a ${build_demos_dir}/aurora-rocksdb/third-party/gflags-2.2.2/build/lib/libgflags.so.2.2.2 libgflags.so
    COMMAND rsync -a ${build_demos_dir}/aurora-rocksdb/third-party/gflags-2.2.2/build/lib/libgflags_nothreads.so.2.2.2 ${build_ramdisk_dir}/libgflags_nothreads.so.2.2
    DEPENDS)

  file(MAKE_DIRECTORY ${build_demos_dir}/aurora-rocksdb/build)
  add_custom_target(
    rocksdb-clean
    COMMAND rm -rf ${build_demos_dir}/aurora-rocksdb/build
    )
    list(APPEND _demos_clean_targets rocksdb-clean)
  add_custom_target(
    rocksdb ALL
    WORKING_DIRECTORY ${build_demos_dir}/aurora-rocksdb/build
    COMMAND cmake -DCMAKE_BUILD_TYPE=Release
            -DCMAKE_C_COMPILER=${MUSL_CC}
            -DCMAKE_CXX_COMPILER=${MUSL_CXX}
            -DCMAKE_CROSSCOMPILING=1
            -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
            -S
            ..
    COMMAND cmake --build . -- -j$(_nproc)
    COMMAND rsync -a ${build_demos_dir}/aurora-rocksdb/build/librocksdb.so.6.* ${build_ramdisk_dir}/librocksdb.so.6
    COMMAND rsync -a ${build_demos_dir}/aurora-rocksdb/build/db_bench ${build_ramdisk_dir}/rocksdb-dbbench
    DEPENDS)
  add_dependencies(rocksdb gflags)
  add_dependencies(ramdisk rocksdb)
endif()

if(CHCORE_DEMOS_PHOENIX)
  set(PHOENIX_DIR ${build_demos_dir}/phoenix-2.0)
  add_subdirectory(${PHOENIX_DIR})
endif()

if(CHCORE_DEMOS_GEMM)
  add_custom_target(
    gemm-clean
    WORKING_DIRECTORY ${build_demos_dir}/GEMM
    COMMAND make clean
  )
  
  list(APPEND _demos_clean_targets gemm-clean)
  add_custom_target(
    gemm ALL
    WORKING_DIRECTORY ${build_demos_dir}/GEMM
    COMMAND make MALLOC=libc CC=${MUSL_CC}
    COMMAND rsync -a ${build_demos_dir}/GEMM/gemm ${build_ramdisk_dir}/gemm
    DEPENDS)
  
  add_custom_target(
    TARGET gemm
    POST_BUILD
    COMMAND rsync -a ${build_demos_dir}/GEMM/gemm ${build_ramdisk_dir}/gemm
  )
  add_dependencies(ramdisk gemm)
endif()

if(CHCORE_DEMOS_LLAMACPP)
  add_custom_target(
    llama-clean
    WORKING_DIRECTORY ${build_demos_dir}/llama.cpp
    COMMAND rm -rf build
  )
  list(APPEND _demos_clean_targets llama-clean)

  add_custom_target(
    llama ALL
    WORKING_DIRECTORY ${build_demos_dir}/llama.cpp
    COMMAND cmake -B build 
      -DCMAKE_C_COMPILER=${MUSL_CC}
      -DCMAKE_CXX_COMPILER=${MUSL_CXX}
      -DCMAKE_CROSSCOMPILING=1 
      -DCMAKE_EXE_LINKER_FLAGS='-L/home/musl-cross-make/install/x86_64-linux-musl/lib -L/home/musl-cross-make/install/lib/gcc/x86_64-linux-musl/9.4.0'
      -DCMAKE_SKIP_RPATH=ON
    COMMAND cmake --build build --config Release -- -j
    COMMENT "Build llama.cpp finished"
    DEPENDS)
  list(APPEND _targets llama)
  add_dependencies(ramdisk llama)

  add_custom_command(
    TARGET llama
    POST_BUILD
    COMMAND rsync -a ${build_demos_dir}/llama.cpp/build/bin/llama-bench ${build_ramdisk_dir}/llama-bench
    COMMAND rsync -a ${build_demos_dir}/llama.cpp/build/bin/*.so ${build_ramdisk_dir}
    COMMENT "Copying llama.cpp files to ramdisk directory"
    DEPENDS llama)
endif()

if(CHCORE_DEMOS_DBX1000)
  add_subdirectory(${build_demos_dir}/dbx1000)
endif()


if (CHCORE_DEMOS_CPYTHON)
if (NOT EXISTS ${libc_install_dir}/lib/libffi.so)
  add_custom_target(
    libffi
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/libffi
    COMMAND ./configure CXX=${MUSL_CXX} CC=${MUSL_CC} --disable-docs --host=x86_64-linux-musl --prefix=${libc_install_dir}
    COMMAND make -j$(_nproc)
    COMMAND make install
    # install libffi.so
    COMMAND rsync -a ${CHCORE_MUSL_LIBC_INSTALL_DIR}/lib/libffi.so.8.1.4 ${build_ramdisk_dir}/libffi.so
    DEPENDS
  )
else()
  add_custom_target(
    libffi
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/libffi
    # install libffi.so
    COMMAND rsync -a ${CHCORE_MUSL_LIBC_INSTALL_DIR}/lib/libffi.so.8.1.4 ${build_ramdisk_dir}/libffi.so
    DEPENDS
  )
endif()

add_custom_target(
    libffi-clean
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/libffi
    COMMAND make clean
)
add_dependencies(ramdisk libffi)

if (NOT EXISTS ${libc_install_dir}/lib/libz.so)
  add_custom_target(
    zlib
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/zlib
    COMMAND ./configure --prefix=${libc_install_dir}
    COMMAND make -j$(_nproc) CC=${MUSL_CC} CXX=${MUSL_CXX}
    COMMAND make install
    # install libz.so
    COMMAND rsync -a ${CHCORE_MUSL_LIBC_INSTALL_DIR}/lib/libz.so.1.3.1 ${build_ramdisk_dir}/libz.so
    DEPENDS
  )
else()
add_custom_target(
    zlib
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/zlib
    # install libz.so
    COMMAND rsync -a ${CHCORE_MUSL_LIBC_INSTALL_DIR}/lib/libz.so.1.3.1 ${build_ramdisk_dir}/libz.so
    DEPENDS
  )
endif()
add_custom_target(
    zlib-clean
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/zlib
    COMMAND make clean
)
add_dependencies(ramdisk zlib)
endif()

if (CHCORE_DEMOS_CPYTHON)
    set(cpython_install_dir ${CMAKE_CURRENT_BINARY_DIR}/cpython)
    if (EXISTS ${cpython_install_dir})
      add_custom_target(
        cpython
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/cpython
        # COMMAND make -j$(_nproc)
        # COMMAND make install prefix=${cpython_install_dir} exec_prefix=${cpython_install_dir}
        COMMAND rsync -a python ${build_ramdisk_dir}/python
        COMMAND rsync -a ${cpython_install_dir}/lib ${build_ramdisk_dir}/
        COMMAND rsync -a example/* ${build_ramdisk_dir}/ --exclude="__pycache__"
        DEPENDS libffi zlib
      )
      add_dependencies(cpython libc-static)
    else()
      add_custom_target(
        cpython
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/cpython
        COMMAND make clean
        COMMAND ./configure 
            CXX=${MUSL_CXX}
            CC=${MUSL_CC}
            LDFLAGS="-L${libc_install_dir}/lib"
            CPPFLAGS="-I${libc_install_dir}/include"
            --host=x86_64-linux-musl
            --build=x86_64-linux
            --disable-ipv6
            --with-build-python
            --with-ensurepip=no
            --prefix=/
            --exec-prefix=/
            ac_cv_file__dev_ptmx=no
            ac_cv_file__dev_ptc=no
        COMMAND make -j$(_nproc)
        COMMAND make install prefix=${cpython_install_dir} exec_prefix=${cpython_install_dir}
        COMMAND rsync -a python ${build_ramdisk_dir}/python
        COMMAND rsync -a ${cpython_install_dir}/lib ${build_ramdisk_dir}/
        COMMAND rsync -a example/* ${build_ramdisk_dir}/ --exclude="__pycache__"
        DEPENDS libffi zlib
      )
      add_dependencies(cpython libc-static)
    endif()
    add_dependencies(ramdisk cpython)

    unset(cpython_install_dir)
endif()

add_custom_target(demos-clean DEPENDS ${_demos_clean_targets} ${_clean_targets})

# add_custom_target(
#   coremark
#   WORKING_DIRECTORY ${build_demos_dir}/coremark
#   COMMAND make PORT_DIR=simple link
#   COMMAND rsync -a coremark.exe ${build_ramdisk_dir}/
# )

# add_dependencies(ramdisk coremark)

if (CHCORE_DEMOS_GEIMINIGRAPH)
  add_subdirectory(${build_demos_dir}/GeminiGraph)
endif()

if (CHCORE_DEMOS_TINYCNN)
  add_subdirectory(${build_demos_dir}/VeryTinyCnn)
endif()
