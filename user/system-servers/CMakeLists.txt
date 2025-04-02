# Process Manager
add_subdirectory(procmgr)

# Shell
add_subdirectory(chcore_shell)

# File System Manager
add_subdirectory(fsm)

# File Systems
add_subdirectory(fs_base)
add_subdirectory(tmpfs)
if(CHCORE_FS_FAT32)
    add_subdirectory(fat32)
endif()
if(CHCORE_FS_EXT4)
    add_subdirectory(ext4)
endif()

# Network Stack
add_subdirectory(lwip)

# Device Drivers
# add_subdirectory(drivers)

# GUI
if(CHCORE_SERVER_GUI)
    add_subdirectory(gui)
    add_subdirectory(terminal)
endif()

# Optional POSIX API Servers
if(CHCORE_SERVER_POSIX_SHM)
    add_subdirectory(posix_shm)
endif()
