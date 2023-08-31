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
            | grep -v "^#" > ${CMAKE_CURRENT_BINARY_DIR}/linker.ld)
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
chcore_generate_emulate_sh(
    "qemu-system-x86_64"
    "--enable-kvm -machine pc,nvdimm=on -m 16G,slots=2,maxmem=64G \
    -object memory-backend-file,id=mem1,share=on,mem-path=\$nvm_backend_file,size=16G \
    -device nvdimm,id=nvdimm1,memdev=mem1 \
    -cpu host -smp 20 -serial mon:stdio -nographic -cdrom \$basedir/chcore.iso"
)
