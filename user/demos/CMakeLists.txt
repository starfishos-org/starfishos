
# demos

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
    COMMAND make MALLOC=libc PCC=${CHCORE_MUSL_CROSS_MAKE_INSTALL_DIR}/bin/x86_64-linux-musl-g++ -s
    COMMAND cp ${build_demos_dir}/ligra/apps/PageRank ${build_ramdisk_dir}/
    COMMAND cp ${build_demos_dir}/ligra/inputs/rMatGraph_J_5_100 ${build_ramdisk_dir}/
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
    COMMAND make MALLOC=libc CC=${CHCORE_MUSL_LIBC_INSTALL_DIR}/bin/musl-gcc -j12 -s
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
    # COMMAND export CC=${CHCORE_MUSL_LIBC_INSTALL_DIR}/bin/musl-gcc
    # COMMAND export CXX=${CHCORE_MUSL_CROSS_MAKE_INSTALL_DIR}/bin/x86_64-linux-musl-g++
    WORKING_DIRECTORY ${build_demos_dir}/YCSB-C/core
    COMMAND make CC=${CHCORE_MUSL_LIBC_INSTALL_DIR}/bin/musl-gcc CXX=${CHCORE_MUSL_CROSS_MAKE_INSTALL_DIR}/bin/x86_64-linux-musl-g++ -s
    WORKING_DIRECTORY ${build_demos_dir}/YCSB-C/redis
    COMMAND make CC=${CHCORE_MUSL_LIBC_INSTALL_DIR}/bin/musl-gcc CXX=${CHCORE_MUSL_CROSS_MAKE_INSTALL_DIR}/bin/x86_64-linux-musl-g++ -s
    WORKING_DIRECTORY ${build_demos_dir}/YCSB-C/db
    COMMAND make CC=${CHCORE_MUSL_LIBC_INSTALL_DIR}/bin/musl-gcc CXX=${CHCORE_MUSL_CROSS_MAKE_INSTALL_DIR}/bin/x86_64-linux-musl-g++ -s
    WORKING_DIRECTORY ${build_demos_dir}/YCSB-C
    COMMAND make CC=${CHCORE_MUSL_LIBC_INSTALL_DIR}/bin/musl-gcc CXX=${CHCORE_MUSL_CROSS_MAKE_INSTALL_DIR}/bin/x86_64-linux-musl-g++ ycsb -s
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
    COMMAND CC=${CHCORE_MUSL_LIBC_INSTALL_DIR}/bin/musl-gcc -s &> /dev/null
    COMMAND make install
)
endif()

if(CHCORE_DEMOS_MEMCACHED)
  add_custom_target(
    memcached ALL
    WORKING_DIRECTORY ${build_demos_dir}/memcached
    COMMAND ./autogen.sh
            && ./configure --disable-coverage -with-libevent=/usr/libevent/ --enable-static
            && make CC=${CHCORE_MUSL_LIBC_INSTALL_DIR}/bin/musl-gcc -s -j12
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
            && make CC=${CHCORE_MUSL_LIBC_INSTALL_DIR}/bin/musl-gcc -s -j12
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
    COMMAND make MALLOC=libc CC=${CHCORE_MUSL_LIBC_INSTALL_DIR}/bin/musl-gcc -j12 -s
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
            -DCMAKE_C_COMPILER=${CHCORE_MUSL_LIBC_INSTALL_DIR}/bin/musl-gcc
            -DCMAKE_CXX_COMPILER=${CHCORE_MUSL_CROSS_MAKE_INSTALL_DIR}/bin/x86_64-linux-musl-g++
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
            -DCMAKE_CXX_COMPILER=${CHCORE_MUSL_CROSS_MAKE_INSTALL_DIR}/bin/x86_64-linux-musl-g++
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
            -DCMAKE_C_COMPILER=${CHCORE_MUSL_LIBC_INSTALL_DIR}/bin/musl-gcc
            -DCMAKE_CXX_COMPILER=${CHCORE_MUSL_CROSS_MAKE_INSTALL_DIR}/bin/x86_64-linux-musl-g++
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
  add_custom_target(
    phoenix-clean
    WORKING_DIRECTORY ${build_demos_dir}/phoenix-2.0
    COMMAND make clean
)
list(APPEND _demos_clean_targets phoenix-clean)
  add_custom_target(
    phoenix ALL
    WORKING_DIRECTORY ${build_demos_dir}/phoenix-2.0
    # FIXME(FN): currently, direct make XX at the root dir will cause errors of finding lib.a
    COMMAND cd ${build_demos_dir}/phoenix-2.0/src
        && make MALLOC=libc CC=${CHCORE_MUSL_LIBC_INSTALL_DIR}/bin/musl-gcc -j4
        && cd ${build_demos_dir}/phoenix-2.0/tests/kmeans
        && make MALLOC=libc CC=${CHCORE_MUSL_LIBC_INSTALL_DIR}/bin/musl-gcc -j4
        && cd ${build_demos_dir}/phoenix-2.0/tests/pca
        && make MALLOC=libc CC=${CHCORE_MUSL_LIBC_INSTALL_DIR}/bin/musl-gcc -j4
        && cd ${build_demos_dir}/phoenix-2.0/tests/word_count
        && make MALLOC=libc CC=${CHCORE_MUSL_LIBC_INSTALL_DIR}/bin/musl-gcc -j4
        && cp ${build_demos_dir}/phoenix-2.0/tests/kmeans/kmeans ${build_ramdisk_dir}/kmeans.bin
        && cp ${build_demos_dir}/phoenix-2.0/tests/pca/pca ${build_ramdisk_dir}/pca.bin
        && cp ${build_demos_dir}/phoenix-2.0/tests/word_count/word_count ${build_ramdisk_dir}/word_count.bin
        && cp ${build_demos_dir}/phoenix-2.0/data/word_count_datafiles/word_10MB.txt ${build_ramdisk_dir}/word_10MB.txt
        # && cp ${build_demos_dir}/phoenix-2.0/data/word_count_datafiles/word_50MB.txt ${build_ramdisk_dir}/word_50MB.txt
        # && cp ${build_demos_dir}/phoenix-2.0/data/word_count_datafiles/word_100MB.txt ${build_ramdisk_dir}/word_100MB.txt
        # && cp ${build_demos_dir}/phoenix-2.0/tests/linear_regression/linear_regression linear_regression.bin
        # && cp ${build_demos_dir}/phoenix-2.0/data/linear_regression_datafiles/key_file_100MB.txt .
        # && cp ${build_demos_dir}/phoenix-2.0/tests/matrix_multiply/matrix_multiply matrix_multiply.bin
        # && cp ${build_demos_dir}/phoenix-2.0/tests/string_match/string_match string_match.bin
        # && cp ${build_demos_dir}/phoenix-2.0/data/string_match_datafiles/key_file_100MB.txt sm_key_file_100MB.txt
        # && cp ${build_demos_dir}/phoenix-2.0/data/histogram_datafiles/med.bmp .
        # && cp ${build_demos_dir}/phoenix-2.0/tests/histogram/histogram histogram.bin
    DEPENDS)
endif()

add_custom_target(demos-clean DEPENDS ${_demos_clean_targets} ${_clean_targets})
