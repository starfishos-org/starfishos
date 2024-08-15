
# demos
set(MUSL_CC ${CHCORE_MUSL_LIBC_INSTALL_DIR}/bin/musl-gcc)
set(MUSL_CXX ${CHCORE_MUSL_CROSS_MAKE_INSTALL_DIR}/bin/x86_64-linux-musl-g++)

set(_demos_clean_targets)

if(CHCORE_DEMOS_LIGRA)
  add_custom_target(
    ligra-clean
    WORKING_DIRECTORY ${build_demos_dir}/ligra/apps
    COMMAND make clean
)
list(APPEND _demos_clean_targets ligra-clean)
  add_custom_target(
    ligra ALL
    WORKING_DIRECTORY ${build_demos_dir}/ligra/apps
    COMMAND make MALLOC=libc OPENMP=1 PCC=${MUSL_CXX}
    COMMAND cp ${build_demos_dir}/ligra/apps/PageRank ${build_ramdisk_dir}/
    # COMMAND cp ${build_demos_dir}/ligra/inputs/* ${build_ramdisk_dir}/
    DEPENDS)
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
    COMMAND make MALLOC=libc CC=${MUSL_CC} -j12 -s
    COMMAND cp ${build_demos_dir}/redis-6.0.8/src/redis-cli ${build_ramdisk_dir}/
    COMMAND cp ${build_demos_dir}/redis-6.0.8/src/redis-server ${build_ramdisk_dir}/
    COMMAND cp ${build_demos_dir}/redis-6.0.8/src/redis-benchmark ${build_ramdisk_dir}/
    DEPENDS)
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
    COMMAND cp /usr/local/lib/libhiredis.so.0.13 . # already builded in docker
    COMMAND cp ${build_demos_dir}/YCSB-C/ycsbc .
    COMMAND cp ${build_demos_dir}/YCSB-C/workloads/*.spec .
    WORKING_DIRECTORY ${build_ramdisk_dir}
    DEPENDS)
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
endif()

if(CHCORE_DEMOS_MEMCACHED)
  add_custom_target(
    memcached ALL
    WORKING_DIRECTORY ${build_demos_dir}/memcached
    COMMAND ./autogen.sh
            && ./configure --disable-coverage -with-libevent=/usr/libevent/ --enable-static
            && make CC=${MUSL_CC} -s -j12
    COMMAND cp ${build_demos_dir}/memcached/memcached ${build_ramdisk_dir}/
    DEPENDS)
    list(APPEND _demos_clean_targets memcached-clean)
  add_custom_target(
    memcached-clean
    COMMAND make maintainer-clean-local
    WORKING_DIRECTORY ${build_demos_dir}/memcached
    )
endif()

if(CHCORE_DEMOS_MEMCACHETEST)
  add_custom_target(
    memcachetest-clean
    WORKING_DIRECTORY ${build_demos_dir}/memcachetest
    COMMAND make distclean
    )
    list(APPEND _demos_clean_targets memcachetest-clean)
  add_custom_target(
    memcachetest ALL
    WORKING_DIRECTORY ${build_demos_dir}/memcachetest
    COMMAND ./config/autorun.sh
            && ./configure --with-libevent-prefix=/usr/libevent/ --enable-static
            && make CC=${MUSL_CC} -s -j12
    COMMAND cp ${build_demos_dir}/memcachetest/memcachetest ${build_ramdisk_dir}/
    DEPENDS)
endif()

if(CHCORE_DEMOS_SQLITE)
  add_custom_target(
    sqlite-clean
    WORKING_DIRECTORY ${build_demos_dir}/sqlite
    COMMAND make clean
)
list(APPEND _demos_clean_targets ycsb-clean)
  add_custom_target(
    sqlite ALL
    WORKING_DIRECTORY ${build_demos_dir}/sqlite
    COMMAND make MALLOC=libc CC=${MUSL_CC} -j12 -s
    COMMAND cp ${build_demos_dir}/sqlite/*.bin ${build_ramdisk_dir}/
    DEPENDS)
endif()

if(CHCORE_DEMOS_LEVELDB)
  add_custom_target(
    leveldb-clean
    WORKING_DIRECTORY ${build_demos_dir}/leveldb-1.23
    COMMAND rm -rf ./build
    )
    list(APPEND _demos_clean_targets ycsb-clean)
  add_custom_target(
    leveldb ALL
    WORKING_DIRECTORY ${build_demos_dir}/leveldb-1.23
    COMMAND mkdir -p build && cd build &&
         cmake -j12 -DCMAKE_BUILD_TYPE=Release
            -DCMAKE_C_COMPILER=${MUSL_CC}
            -DCMAKE_CXX_COMPILER=${MUSL_CXX}
            -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
            -DCMAKE_CROSSCOMPILING=1 -DRUN_HAVE_STD_REGEX=0 -DRUN_HAVE_POSIX_REGEX=0 -S ..
            && cmake --build . -j12
    COMMAND cp ${build_demos_dir}/leveldb-1.23/build/db_bench ${build_ramdisk_dir}/leveldb-dbbench
    DEPENDS)
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
    COMMAND cmake -j12 -DCMAKE_BUILD_TYPE=Release
            -DCMAKE_CXX_COMPILER=${MUSL_CXX}
            -DCMAKE_CXX_FLAGS=-fPIC
            -DCMAKE_CROSSCOMPILING=1
            -DBUILD_SHARED_LIBS=ON
            # -DBUILD_SHARED_LIBS=OFF
            -DBUILD_TESTING=OFF
            -S ..
            && cmake --build . -j12
    # COMMAND cp ${build_demos_dir}/aurora-rocksdb/third-party/gflags-2.2.2/build/lib/libgflags.so.2.2.2 libgflags.so
    COMMAND cp ${build_demos_dir}/aurora-rocksdb/third-party/gflags-2.2.2/build/lib/libgflags_nothreads.so.2.2.2 ${build_ramdisk_dir}/libgflags_nothreads.so.2.2
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
    COMMAND cmake -j12 -DCMAKE_BUILD_TYPE=Release
            -DCMAKE_C_COMPILER=${MUSL_CC}
            -DCMAKE_CXX_COMPILER=${MUSL_CXX}
            -DCMAKE_CROSSCOMPILING=1
            -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
            -S
            ..
    COMMAND cmake --build . -j12
    COMMAND cp ${build_demos_dir}/aurora-rocksdb/build/librocksdb.so.6.* ${build_ramdisk_dir}/librocksdb.so.6
    COMMAND cp ${build_demos_dir}/aurora-rocksdb/build/db_bench ${build_ramdisk_dir}/rocksdb-dbbench
    COMMAND ls ${build_ramdisk_dir}
    DEPENDS)
  add_dependencies(rocksdb gflags)
endif()

if(CHCORE_DEMOS_PHOENIX)
  # Define variables for commonly used paths
  set(PHOENIX_DIR ${build_demos_dir}/phoenix-2.0)
  set(PHOENIX_SRC_DIR ${PHOENIX_DIR}/src)
  set(PHOENIX_TESTS_DIR ${PHOENIX_DIR}/tests)

  # Add the custom clean target
  add_custom_target(
    phoenix-clean
    WORKING_DIRECTORY ${PHOENIX_DIR}
    COMMAND make clean
  )
  list(APPEND _demos_clean_targets phoenix-clean)

  # Define the list of tests to build
  set(TESTS kmeans pca word_count matrix_multiply basic)

  # Add the custom target for building Phoenix
  add_custom_target(
      phoenix ALL
      WORKING_DIRECTORY ${PHOENIX_DIR}
      COMMAND cd ${PHOENIX_SRC_DIR} && make MALLOC=libc CC=${MUSL_CC} -j4
      DEPENDS
  )

  # Loop through each test and add the build commands
  # foreach(test ${TESTS})
  #     add_custom_command(
  #         TARGET phoenix
  #         POST_BUILD
  #         COMMAND cd ${PHOENIX_TESTS_DIR}/${test} && make MALLOC=libc CC=${MUSL_CC} -j4
  #         COMMAND cp ${PHOENIX_TESTS_DIR}/${test}/${test} ${build_ramdisk_dir}/${test}.bin
  #     )
  # endforeach()

  add_custom_command(
    TARGET phoenix
    POST_BUILD
    COMMAND cd ${PHOENIX_TESTS_DIR}/${test}/matrix_multiply && make MALLOC=libc CC=${MUSL_CC} -j4
    COMMAND cp ${PHOENIX_TESTS_DIR}/matrix_multiply/matrix_multiply ${build_ramdisk_dir}/matrix_multiply.bin
    COMMAND cp ${PHOENIX_TESTS_DIR}/matrix_multiply/matrix_multiply_cxl ${build_ramdisk_dir}/matrix_multiply_cxl.bin
  )
  # Copy additional data files
  add_custom_command(
      TARGET phoenix
      POST_BUILD
      # COMMAND cp ${PHOENIX_DIR}/data/word_count_datafiles/word_10MB.txt ${build_ramdisk_dir}/word_10MB.txt
      COMMAND cp ${PHOENIX_DIR}/data/matrix_datafiles/matrix_file_A.txt ${build_ramdisk_dir}/
      COMMAND cp ${PHOENIX_DIR}/data/matrix_datafiles/matrix_file_B.txt ${build_ramdisk_dir}/
  )
endif()

add_custom_target(demos-clean DEPENDS ${_demos_clean_targets} ${_clean_targets})
