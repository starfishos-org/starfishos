# Add bootloader source files
chcore_target_sources_out_objects(${kernel_target} PRIVATE init_objects
                                  init/header.S init/gdt.c)
message(STATUS "init_objects: ${init_objects}")
string(REGEX REPLACE ";" "\n" init_objects "${init_objects}")

# Prepare and set linker script
configure_file(linker.tpl.ld linker.ld.S)
add_custom_target(
    linker.ld ALL
    COMMAND ${CMAKE_C_COMPILER} -E -x c ${CMAKE_CURRENT_BINARY_DIR}/linker.ld.S
            | grep -v "^\#" > ${CMAKE_CURRENT_BINARY_DIR}/linker.ld)
add_dependencies(${kernel_target} linker.ld)
target_link_options(${kernel_target} PRIVATE -T
                    ${CMAKE_CURRENT_BINARY_DIR}/linker.ld -z max-page-size=4096)

# Make ISO image
add_custom_target(
    chcore.iso ALL
    COMMAND [ -f chcore.iso ] && rm -f chcore.iso || true
    COMMAND [ -d iso ] && rm -rf iso || true
    COMMAND cp -r ${CMAKE_CURRENT_SOURCE_DIR}/iso .
    COMMAND cp $<TARGET_FILE:${kernel_target}> iso/boot
    COMMAND grub-mkrescue -o chcore.iso iso
    COMMAND test -f chcore.iso
    DEPENDS ${kernel_target})
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/chcore.iso
        DESTINATION ${CMAKE_INSTALL_PREFIX})

# Note: `$basedir` is a shell variable in `emulate.tpl.sh`
if(0)
  chcore_generate_emulate_sh(
    "qemu-system-x86_64"
    "--enable-kvm -machine pc,nvdimm=on -m 16G,slots=2,maxmem=64G \
    -object memory-backend-file,id=mem1,share=on,mem-path=\$nvm_backend_file,size=16G \
    -device nvdimm,id=nvdimm1,memdev=mem1 \
    -cpu host -smp 20 -serial mon:stdio -nographic -cdrom \$basedir/chcore.iso"
  )
endif()

    # "--trace events=\$basedir/../qemu-trace-events.txt \
    # -object memory-backend-file,id=vmem0,share=on,mem-path=\$cxl_backend_file,size=1G \
chcore_generate_emulate_sh(
    "/opt/qemu-jic23/bin/qemu-system-x86_64"
    "--enable-kvm -M q35,cxl=on -m 4G,slots=2,maxmem=64G \
    -object memory-backend-ram,id=vmem0,share=on,size=1G \
    -device pxb-cxl,bus_nr=12,bus=pcie.0,id=cxl.1,hdm_for_passthrough=true \
    -device cxl-rp,port=0,bus=cxl.1,id=root_port13,chassis=0,slot=0 \
    -device cxl-type3,bus=root_port13,volatile-memdev=vmem0,id=cxl-vmem0 \
    -M cxl-fmw.0.targets.0=cxl.1,cxl-fmw.0.size=1G \
    -cpu host -smp 10 -serial mon:stdio -nographic -cdrom \$basedir/chcore.iso"
  )

if(0)
  chcore_generate_emulate_sh(
    "/opt/qemu-jic23/bin/qemu-system-x86_64"
    "--enable-kvm -M q35,cxl=on -m 4G,slots=2,maxmem=64G \
    -object memory-backend-file,id=cxl-mem1,share=on,mem-path=/tmp/cxltest.raw,size=256M \
    -object memory-backend-file,id=cxl-lsa1,share=on,mem-path=/tmp/lsa.raw,size=256M \
    -device pxb-cxl,bus_nr=12,bus=pcie.0,id=cxl.1,hdm_for_passthrough=true \
    -device cxl-rp,port=0,bus=cxl.1,id=root_port13,chassis=0,slot=2 \
    -device cxl-type3,bus=root_port13,persistent-memdev=cxl-mem1,lsa=cxl-lsa1,id=cxl-pmem0 \
    -M cxl-fmw.0.targets.0=cxl.1,cxl-fmw.0.size=4G \
    -cpu host -smp 20 -serial mon:stdio -nographic -cdrom \$basedir/chcore.iso"
  )


  chcore_generate_emulate_sh(
  "qemu-system-x86_64"
  "--enable-kvm -M q35 -m 32G \
  -machine memory-backend=mem \
  -object memory-backend-file,id=mem,size=32G,share=on,mem-path=/dev/shm/large-shmem \
  -cpu host -smp 10 -serial mon:stdio -nographic -cdrom \$basedir/chcore.iso"
)

  chcore_generate_emulate_sh(
  "qemu-system-x86_64"
  "--enable-kvm -M q35 -m 32G \
  -machine memory-backend=mem \
  -object memory-backend-file,id=mem,size=32G,share=on,mem-path=/dev/shm/large-shmem \
  -cpu host -smp 10 -serial mon:stdio -nographic -cdrom \$basedir/chcore.iso"
)
endif()
