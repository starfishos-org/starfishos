
# demos
set(MUSL_CC ${CHCORE_MUSL_LIBC_INSTALL_DIR}/bin/musl-gcc)
set(MUSL_CXX ${CHCORE_MUSL_CROSS_MAKE_INSTALL_DIR}/bin/x86_64-linux-musl-g++)
set(CMAKE_CXX_COMPILER ${MUSL_CXX})
set(CMAKE_C_COMPILER ${MUSL_CC})

set(_demos_clean_targets)
chcore_get_nproc(_nproc)

set(OS CHCORE)
set(_demos_materialize_git_tree
    ${CMAKE_CURRENT_LIST_DIR}/../../scripts/materialize-git-tree.sh)

# These legacy options can remain in an existing .config or CMake cache, but
# their source checkouts and build integrations are not part of this tree.
# Refuse an explicit ON instead of reporting a successful build with no output.
if(CHCORE_DEMOS_LLAMACPP)
  message(FATAL_ERROR
    "CHCORE_DEMOS_LLAMACPP is ON, but the llama.cpp checkout/build support "
    "is not included in this repository")
endif()
if(CHCORE_DEMOS_CPYTHON)
  message(FATAL_ERROR
    "CHCORE_DEMOS_CPYTHON is ON, but the CPython checkout/build support "
    "is not included in this repository")
endif()

# Fail during configuration with an actionable message instead of letting a
# later add_subdirectory(), make, or rsync fail inside an empty submodule path.
function(chcore_require_demo_checkout option directory marker)
  if(${option} AND
     NOT EXISTS "${src_demos_dir}/${directory}/${marker}")
    message(FATAL_ERROR
      "${directory} is enabled but its submodule is not initialized. Run: "
      "git submodule update --init --recursive -- user/demos/${directory}")
  endif()
endfunction()

chcore_require_demo_checkout(CHCORE_DEMOS_REDIS redis-6.0.8 Makefile)
chcore_require_demo_checkout(CHCORE_DEMOS_YCSB YCSB-C Makefile)
chcore_require_demo_checkout(CHCORE_DEMOS_MEMCACHED memcached Makefile)
chcore_require_demo_checkout(CHCORE_DEMOS_MEMCACHETEST memcachetest Makefile)
chcore_require_demo_checkout(CHCORE_DEMOS_SQLITE sqlite Makefile)
chcore_require_demo_checkout(CHCORE_DEMOS_LEVELDB leveldb-1.23 CMakeLists.txt)
chcore_require_demo_checkout(CHCORE_DEMOS_ROCKSDB aurora-rocksdb CMakeLists.txt)
chcore_require_demo_checkout(CHCORE_DEMOS_PHOENIX phoenix-2.0 CMakeLists.txt)
chcore_require_demo_checkout(CHCORE_DEMOS_DBX1000 dbx1000 CMakeLists.txt)
chcore_require_demo_checkout(CHCORE_DEMOS_GEIMINIGRAPH GeminiGraph CMakeLists.txt)
chcore_require_demo_checkout(CHCORE_DEMOS_TINYCNN VeryTinyCnn CMakeLists.txt)
chcore_require_demo_checkout(CHCORE_DEMOS_TINYCNN VeryTinyCnn/libjpeg configure)

if(CHCORE_DEMOS_LIGRA)
  add_subdirectory(${src_demos_dir}/ligra)
endif()

if(CHCORE_DEMOS_REDIS)
  set(REDIS_SOURCE_DIR ${src_demos_dir}/redis-6.0.8)
  set(REDIS_BUILD_DIR ${CMAKE_CURRENT_BINARY_DIR}/redis-6.0.8)
  add_custom_target(
    redis-clean
    COMMAND ${CMAKE_COMMAND} -E remove_directory ${REDIS_BUILD_DIR}
)
list(APPEND _demos_clean_targets redis-clean)
  add_custom_target(
    redis ALL
    COMMAND bash ${_demos_materialize_git_tree}
            ${REDIS_SOURCE_DIR} ${REDIS_BUILD_DIR}
    COMMAND make -C ${REDIS_BUILD_DIR} -B MALLOC=libc CC=${MUSL_CC}
    COMMAND rsync -a ${REDIS_BUILD_DIR}/src/redis-cli ${build_ramdisk_dir}/redis-cli
    COMMAND rsync -a ${REDIS_BUILD_DIR}/src/redis-server ${build_ramdisk_dir}/redis-server
    COMMAND rsync -a ${REDIS_BUILD_DIR}/src/redis-benchmark ${build_ramdisk_dir}/redis-benchmark
    COMMAND rsync -a ${REDIS_BUILD_DIR}/redis.conf ${build_ramdisk_dir}/redis.conf
    DEPENDS)
  add_dependencies(ramdisk redis)
endif()

if(CHCORE_DEMOS_YCSB)
  set(YCSB_SOURCE_DIR ${src_demos_dir}/YCSB-C)
  set(YCSB_BUILD_DIR ${CMAKE_CURRENT_BINARY_DIR}/YCSB-C)
  add_custom_target(
    ycsb-clean
    COMMAND ${CMAKE_COMMAND} -E remove_directory ${YCSB_BUILD_DIR}
)
list(APPEND _demos_clean_targets ycsb-clean)
  add_custom_target(
    ycsb ALL
    COMMAND bash ${_demos_materialize_git_tree}
            ${YCSB_SOURCE_DIR} ${YCSB_BUILD_DIR}
    COMMAND ${CMAKE_COMMAND} -E env
            "LD_LIBRARY_PATH=$ENV{LD_LIBRARY_PATH}:/usr/local/lib"
            make -C ${YCSB_BUILD_DIR}/core CC=${MUSL_CC} CXX=${MUSL_CXX} -s
    COMMAND ${CMAKE_COMMAND} -E env
            "LD_LIBRARY_PATH=$ENV{LD_LIBRARY_PATH}:/usr/local/lib"
            make -C ${YCSB_BUILD_DIR}/db CC=${MUSL_CC} CXX=${MUSL_CXX} -s
    COMMAND ${CMAKE_COMMAND} -E env
            "LD_LIBRARY_PATH=$ENV{LD_LIBRARY_PATH}:/usr/local/lib"
            make -C ${YCSB_BUILD_DIR}/redis CC=${MUSL_CC} CXX=${MUSL_CXX} -s
    COMMAND ${CMAKE_COMMAND} -E env
            "LD_LIBRARY_PATH=$ENV{LD_LIBRARY_PATH}:/usr/local/lib"
            make -C ${YCSB_BUILD_DIR} CC=${MUSL_CC} CXX=${MUSL_CXX} ycsb -s
)

  add_custom_command(
    TARGET ycsb
    POST_BUILD
    COMMAND rsync -a /usr/local/lib/libhiredis.so.0.13 . # already builded in docker
    COMMAND rsync -a ${YCSB_BUILD_DIR}/ycsbc .
    COMMAND rsync -a ${YCSB_BUILD_DIR}/workloads/*.spec .
    WORKING_DIRECTORY ${build_ramdisk_dir}
    DEPENDS)
  add_dependencies(ramdisk ycsb)
endif()

# libevent is already installed in docker images
if(CHCORE_DEMOS_LIBEVENT)
  add_custom_target(
    libevent ALL
    WORKING_DIRECTORY ${src_demos_dir}/libevent
    COMMAND ./autogen.sh
    COMMAND ./configure --disable-openssl --disable-debug-mode --disable-samples -prefix=/usr/libevent
    COMMAND CC=${MUSL_CC} -s &> /dev/null
    COMMAND make install
  )
  add_dependencies(ramdisk libevent)
endif()

if(CHCORE_DEMOS_MEMCACHED)
  set(MEMCACHED_SOURCE_DIR ${src_demos_dir}/memcached)
  set(MEMCACHED_BUILD_DIR ${CMAKE_CURRENT_BINARY_DIR}/memcached)

  add_custom_target(memcached ALL
    COMMAND bash ${_demos_materialize_git_tree}
            ${MEMCACHED_SOURCE_DIR} ${MEMCACHED_BUILD_DIR}
    COMMAND ${CMAKE_COMMAND} -E chdir ${MEMCACHED_BUILD_DIR} ./autogen.sh
    COMMAND ${CMAKE_COMMAND} -E chdir ${MEMCACHED_BUILD_DIR}
            ${CMAKE_COMMAND} -E env CC=${MUSL_CC} AWK=awk
            ./configure
            --build=x86_64-pc-linux-gnu
            --host=x86_64-linux-musl
            --disable-coverage
            --disable-docs
            --with-libevent=/usr/libevent
            --enable-static
    COMMAND make -C ${MEMCACHED_BUILD_DIR} CC=${MUSL_CC} AWK=awk
    COMMENT "Building memcached"
  )

  add_custom_target(
    memcached-copy-data
    COMMAND rsync -a ${MEMCACHED_BUILD_DIR}/memcached ${build_ramdisk_dir}/memcached
    DEPENDS memcached
    COMMENT "Copying memcached to ramdisk"
  )

  add_dependencies(ramdisk memcached-copy-data)

  add_custom_target(memcached-clean
    COMMAND ${CMAKE_COMMAND} -E remove_directory ${MEMCACHED_BUILD_DIR}
    COMMENT "Cleaning memcached"
  )

  list(APPEND _demos_clean_targets memcached-clean)
  add_dependencies(ramdisk memcached)
endif()

if(CHCORE_DEMOS_MEMCACHETEST)
  set(MEMCACHETEST_SOURCE_DIR ${src_demos_dir}/memcachetest)
  set(MEMCACHETEST_BUILD_DIR ${CMAKE_CURRENT_BINARY_DIR}/memcachetest)

  # 编译阶段
  add_custom_target(memcachetest ALL
    COMMAND bash ${_demos_materialize_git_tree}
            ${MEMCACHETEST_SOURCE_DIR} ${MEMCACHETEST_BUILD_DIR}
    COMMAND ${CMAKE_COMMAND} -E chdir ${MEMCACHETEST_BUILD_DIR} autoreconf -fi
    COMMAND ${CMAKE_COMMAND} -E chdir ${MEMCACHETEST_BUILD_DIR}
            ${CMAKE_COMMAND} -E env
            CC=${MUSL_CC}
            CXX=${MUSL_CXX}
            AWK=awk
            CPPFLAGS=-I/usr/libevent/include
            "LDFLAGS=-static -L/usr/libevent/lib"
            LIBS=-levent
            ac_cv_func_clock_gettime=yes
            ac_cv_func_malloc_0_nonnull=yes
            ac_cv_func_realloc_0_nonnull=yes
            pandora_cv_use_pipe=yes
            ./configure
            --build=x86_64-pc-linux-gnu
            --host=x86_64-linux-musl
            --with-libevent-prefix=/usr/libevent
            --enable-static
    COMMAND make -C ${MEMCACHETEST_BUILD_DIR} CC=${MUSL_CC} AWK=awk
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
    COMMAND ${CMAKE_COMMAND} -E remove_directory ${MEMCACHETEST_BUILD_DIR}
    COMMENT "Cleaning memcachetest"
  )

  list(APPEND _demos_clean_targets memcachetest-clean)
  add_dependencies(ramdisk memcachetest)
endif()

if(CHCORE_DEMOS_SQLITE)
  set(SQLITE_SOURCE_DIR ${src_demos_dir}/sqlite)
  set(SQLITE_BUILD_DIR ${CMAKE_CURRENT_BINARY_DIR}/sqlite)
  add_custom_target(
    sqlite-clean
    COMMAND ${CMAKE_COMMAND} -E remove_directory ${SQLITE_BUILD_DIR}
)
list(APPEND _demos_clean_targets sqlite-clean)
  add_custom_target(
    sqlite ALL
    COMMAND bash ${_demos_materialize_git_tree}
            ${SQLITE_SOURCE_DIR} ${SQLITE_BUILD_DIR}
    COMMAND make -C ${SQLITE_BUILD_DIR} MALLOC=libc CC=${MUSL_CC} -j$(_nproc) -s
    COMMAND rsync -a ${SQLITE_BUILD_DIR}/*.bin ${build_ramdisk_dir}/
    DEPENDS)
endif()

if(CHCORE_DEMOS_LEVELDB)
  set(LEVELDB_DIR ${src_demos_dir}/leveldb-1.23)
  add_subdirectory(${LEVELDB_DIR} ${CMAKE_CURRENT_BINARY_DIR}/leveldb-1.23)
endif()

if(CHCORE_DEMOS_ROCKSDB)
  set(ROCKSDB_SOURCE_DIR ${src_demos_dir}/aurora-rocksdb)
  set(ROCKSDB_BUILD_DIR ${CMAKE_CURRENT_BINARY_DIR}/aurora-rocksdb)
  set(GFLAGS_SOURCE_DIR
      ${ROCKSDB_SOURCE_DIR}/third-party/gflags-2.2.2)
  set(GFLAGS_BUILD_DIR ${CMAKE_CURRENT_BINARY_DIR}/aurora-rocksdb-gflags)

  add_custom_target(
    gflags-clean
    COMMAND ${CMAKE_COMMAND} -E remove_directory ${GFLAGS_BUILD_DIR}
    )
    list(APPEND _demos_clean_targets gflags-clean)
  add_custom_target(
    gflags ALL
    COMMAND ${CMAKE_COMMAND}
            -S ${GFLAGS_SOURCE_DIR}
            -B ${GFLAGS_BUILD_DIR}
            -DCMAKE_BUILD_TYPE=Release
            -DCMAKE_CXX_COMPILER=${MUSL_CXX}
            -DCMAKE_CXX_FLAGS=-fPIC
            -DCMAKE_CROSSCOMPILING=1
            -DBUILD_SHARED_LIBS=ON
            -DBUILD_TESTING=OFF
    COMMAND ${CMAKE_COMMAND} --build ${GFLAGS_BUILD_DIR} -- -j$(_nproc)
    COMMAND rsync -a ${GFLAGS_BUILD_DIR}/lib/libgflags_nothreads.so.2.2.2 ${build_ramdisk_dir}/libgflags_nothreads.so.2.2
    DEPENDS)

  add_custom_target(
    rocksdb-clean
    COMMAND ${CMAKE_COMMAND} -E remove_directory ${ROCKSDB_BUILD_DIR}
    )
    list(APPEND _demos_clean_targets rocksdb-clean)
  add_custom_target(
    rocksdb ALL
    COMMAND ${CMAKE_COMMAND}
            -S ${ROCKSDB_SOURCE_DIR}
            -B ${ROCKSDB_BUILD_DIR}
            -DCMAKE_BUILD_TYPE=Release
            -DCMAKE_C_COMPILER=${MUSL_CC}
            -DCMAKE_CXX_COMPILER=${MUSL_CXX}
            -DCMAKE_CROSSCOMPILING=1
            -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
            -DCMAKE_CXX_FLAGS=-I${GFLAGS_BUILD_DIR}/include
            -DCMAKE_EXE_LINKER_FLAGS=-L${GFLAGS_BUILD_DIR}/lib
            -DCMAKE_SHARED_LINKER_FLAGS=-L${GFLAGS_BUILD_DIR}/lib
    COMMAND ${CMAKE_COMMAND} --build ${ROCKSDB_BUILD_DIR} -- -j$(_nproc)
    COMMAND rsync -a ${ROCKSDB_BUILD_DIR}/librocksdb.so.6.* ${build_ramdisk_dir}/librocksdb.so.6
    COMMAND rsync -a ${ROCKSDB_BUILD_DIR}/db_bench ${build_ramdisk_dir}/rocksdb-dbbench
    DEPENDS)
  add_dependencies(rocksdb gflags)
  add_dependencies(ramdisk rocksdb)
endif()

if(CHCORE_DEMOS_PHOENIX)
  set(PHOENIX_DIR ${src_demos_dir}/phoenix-2.0)
  set(PHOENIX_TIMING ON CACHE BOOL "" FORCE)
  add_subdirectory(${PHOENIX_DIR} ${CMAKE_CURRENT_BINARY_DIR}/phoenix-2.0)

  # Phoenix's worker pool uses detached pthreads and considers them destroyed
  # as soon as their loop posts sem_all_workers_done. On ChCore that signal is
  # sent before musl has removed the worker from its thread/TSD list. The main
  # thread can then enter pthread_key_delete(), follow a worker whose detached
  # mapping has already gone away, and fault during benchmark finalization.
  #
  # Keep the upstream submodule clean: generate a ChCore-only tpool.c with
  # joinable workers and an explicit join before key deletion, then replace
  # only that source in phoenix-lib.
  set(_phoenix_tpool_src "${PHOENIX_DIR}/src/tpool.c")
  set(_phoenix_tpool_fixed "${CMAKE_CURRENT_BINARY_DIR}/phoenix-tpool-chcore.c")
  file(READ "${_phoenix_tpool_src}" _phoenix_tpool_contents)

  set(_phoenix_detached
      "pthread_attr_setdetachstate (&attr, PTHREAD_CREATE_DETACHED)")
  set(_phoenix_joinable
      "pthread_attr_setdetachstate (&attr, PTHREAD_CREATE_JOINABLE)")
  string(FIND "${_phoenix_tpool_contents}" "${_phoenix_detached}"
         _phoenix_detached_pos)
  if(_phoenix_detached_pos EQUAL -1)
    message(FATAL_ERROR
            "Phoenix tpool layout changed: detached-worker override no longer applies")
  endif()
  string(REPLACE "${_phoenix_detached}" "${_phoenix_joinable}"
         _phoenix_tpool_contents "${_phoenix_tpool_contents}")

  set(_phoenix_wait_old
      "sem_wait(&tpool->sem_all_workers_done);\n\n    sem_destroy")
  set(_phoenix_wait_new
      "sem_wait(&tpool->sem_all_workers_done);\n\n    /* Wait until musl has unlinked every worker from its TSD list. */\n    for (i = 0; i < tpool->num_threads; ++i) {\n        CHECK_ERROR (pthread_join (tpool->threads[i], NULL));\n    }\n\n    sem_destroy")
  string(FIND "${_phoenix_tpool_contents}" "${_phoenix_wait_old}"
         _phoenix_wait_pos)
  if(_phoenix_wait_pos EQUAL -1)
    message(FATAL_ERROR
            "Phoenix tpool layout changed: worker-join override no longer applies")
  endif()
  string(REPLACE "${_phoenix_wait_old}" "${_phoenix_wait_new}"
         _phoenix_tpool_contents "${_phoenix_tpool_contents}")
  file(WRITE "${_phoenix_tpool_fixed}" "${_phoenix_tpool_contents}")

  get_target_property(_phoenix_sources phoenix-lib SOURCES)
  list(FILTER _phoenix_sources EXCLUDE REGEX "[/\\\\]tpool[.]c$")
  list(APPEND _phoenix_sources "${_phoenix_tpool_fixed}")
  set_property(TARGET phoenix-lib PROPERTY SOURCES "${_phoenix_sources}")
endif()

if(CHCORE_DEMOS_GEMM)
  add_custom_target(
    gemm-clean
    WORKING_DIRECTORY ${src_demos_dir}/GEMM
    COMMAND make clean
  )
  
  list(APPEND _demos_clean_targets gemm-clean)
  add_custom_target(
    gemm ALL
    WORKING_DIRECTORY ${src_demos_dir}/GEMM
    COMMAND make MALLOC=libc CC=${MUSL_CC}
    COMMAND rsync -a ${src_demos_dir}/GEMM/gemm ${build_ramdisk_dir}/gemm
    DEPENDS)
  
  add_custom_target(
    TARGET gemm
    POST_BUILD
    COMMAND rsync -a ${src_demos_dir}/GEMM/gemm ${build_ramdisk_dir}/gemm
  )
  add_dependencies(ramdisk gemm)
endif()

if(CHCORE_DEMOS_DBX1000)
  add_subdirectory(${src_demos_dir}/dbx1000
                   ${CMAKE_CURRENT_BINARY_DIR}/dbx1000)
endif()

# add_custom_target(
#   coremark
#   WORKING_DIRECTORY ${src_demos_dir}/coremark
#   COMMAND make PORT_DIR=simple link
#   COMMAND rsync -a coremark.exe ${build_ramdisk_dir}/
# )

# add_dependencies(ramdisk coremark)

if (CHCORE_DEMOS_GEIMINIGRAPH)
  add_subdirectory(${src_demos_dir}/GeminiGraph
                   ${CMAKE_CURRENT_BINARY_DIR}/GeminiGraph)
endif()

if (CHCORE_DEMOS_TINYCNN)
  set(TINYCNN_SOURCE_DIR ${src_demos_dir}/VeryTinyCnn)
  set(TINYCNN_LIBJPEG_SOURCE_DIR ${TINYCNN_SOURCE_DIR}/libjpeg)
  set(TINYCNN_LIBJPEG_BUILD_DIR
      ${CMAKE_CURRENT_BINARY_DIR}/VeryTinyCnn-libjpeg)
  set(TINYCNN_RAW_FILELIST
      ${CMAKE_CURRENT_BINARY_DIR}/VeryTinyCnn-raw_filelists.txt)

  add_custom_target(
    tinycnn-clean
    COMMAND ${CMAKE_COMMAND} -E remove_directory ${TINYCNN_LIBJPEG_BUILD_DIR}
    COMMAND ${CMAKE_COMMAND} -E remove -f ${TINYCNN_RAW_FILELIST}
  )
  list(APPEND _demos_clean_targets tinycnn-clean)

  add_custom_target(
    libjpeg-build
    COMMAND bash ${_demos_materialize_git_tree}
            ${TINYCNN_LIBJPEG_SOURCE_DIR} ${TINYCNN_LIBJPEG_BUILD_DIR}
    COMMAND ${CMAKE_COMMAND} -E chdir ${TINYCNN_LIBJPEG_BUILD_DIR}
            ${CMAKE_COMMAND} -E env CC=${CMAKE_C_COMPILER} AWK=awk
            ./configure
            --build=x86_64-pc-linux-gnu
            --host=x86_64-linux-musl
            --disable-shared
            --enable-static
    COMMAND make -C ${TINYCNN_LIBJPEG_BUILD_DIR}
  )

  add_library(libjpeg STATIC IMPORTED)
  set_property(TARGET libjpeg PROPERTY IMPORTED_LOCATION
               ${TINYCNN_LIBJPEG_BUILD_DIR}/.libs/libjpeg.a)

  add_executable(tiny-cnn ${TINYCNN_SOURCE_DIR}/feature.cpp)
  target_link_options(tiny-cnn PRIVATE -static)
  target_link_libraries(tiny-cnn PRIVATE
                        libjpeg rpmalloc-static libc-static libstdc++-static)
  target_include_directories(tiny-cnn PRIVATE
                             ${TINYCNN_LIBJPEG_BUILD_DIR}
                             ${TINYCNN_SOURCE_DIR}/include)
  target_compile_definitions(tiny-cnn PRIVATE CHCORE cimg_display=0)
  target_compile_options(tiny-cnn PRIVATE -mavx2)
  add_dependencies(tiny-cnn libjpeg-build)

  add_custom_target(
    tiny-cnn-copy-data
    COMMAND /bin/sh -c
            "cut -f 1 '${TINYCNN_SOURCE_DIR}/data/filelists.txt'>'${TINYCNN_RAW_FILELIST}'"
    COMMAND rsync -a $<TARGET_FILE:tiny-cnn> ${build_ramdisk_dir}/
    COMMAND rsync -a ${TINYCNN_SOURCE_DIR}/data/alexnet.dat ${build_ramdisk_dir}/
    COMMAND rsync -a ${TINYCNN_SOURCE_DIR}/data/pca/nn-004.dat ${build_ramdisk_dir}/
    COMMAND rsync -a ${TINYCNN_SOURCE_DIR}/image ${build_ramdisk_dir}/
    COMMAND rsync -a ${TINYCNN_RAW_FILELIST} ${build_ramdisk_dir}/
    DEPENDS tiny-cnn
  )
  add_dependencies(ramdisk tiny-cnn-copy-data)
endif()

add_custom_target(demos-clean DEPENDS ${_demos_clean_targets} ${_clean_targets})
