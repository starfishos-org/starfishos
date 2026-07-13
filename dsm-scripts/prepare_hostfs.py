#!/usr/bin/env python3
import mmap
import os
import struct
import sys

base_dir = "/dev/shm"

# 要复制到共享内存的文件路径
source_file_list = [
    # '/disk/wfn/models/Meta-Llama-3-8B-Instruct.Q5_K_M.gguf',
    '/mnt/disk1/wfn/twitter-2010/twitter-2010.bin',
    # '/disk/xt/models/Meta-Llama-3.1-8B-Instruct-Q8_0.gguf',
    # '/disk/yjs/model/Llama-3.2-1B-Instruct-f16.gguf'
]

# 共享内存设备文件路径
# get current user name
user_name = os.getenv('USER')
shm_device_path = f'{base_dir}/ivshmem-hostfs-{user_name}'

# 头部信息，格式为
file_info_list= []

PAGE_SIZE = 4096

def round_up(x, n):
    return ((x) + (n) - 1) & ~((n) - 1)


def hostfs_metadata_is_current():
    """Return whether hostfs already describes the configured source files."""
    if not os.path.isfile(shm_device_path):
        return False

    expected_files = []
    for source_file_path in source_file_list:
        if os.path.isfile(source_file_path):
            expected_files.append((
                os.path.basename(source_file_path),
                os.path.getsize(source_file_path),
            ))

    try:
        with open(shm_device_path, 'rb') as shm_fd:
            if shm_fd.read(8) != b"hostfs\0\0":
                return False

            file_num_data = shm_fd.read(8)
            if len(file_num_data) != 8:
                return False
            file_num = struct.unpack("<Q", file_num_data)[0]
            if file_num != len(expected_files):
                return False

            offset = PAGE_SIZE
            for expected_name, expected_size in expected_files:
                entry = shm_fd.read(8 + 8 + 128)
                if len(entry) != 8 + 8 + 128:
                    return False
                file_offset, file_size = struct.unpack("<QQ", entry[:16])
                file_name = entry[16:].split(b'\0', 1)[0].decode('utf-8')
                if (file_offset, file_size, file_name) != (offset, expected_size, expected_name):
                    return False
                offset += round_up(expected_size, PAGE_SIZE)
    except (OSError, UnicodeDecodeError, struct.error):
        return False

    return True


if len(sys.argv) == 2 and sys.argv[1] == "--check":
    if hostfs_metadata_is_current():
        print(f"Hostfs metadata is current: {shm_device_path}")
        sys.exit(0)
    print(f"Hostfs metadata is missing or stale: {shm_device_path}")
    sys.exit(1)
elif len(sys.argv) != 1:
    print(f"Usage: {sys.argv[0]} [--check]", file=sys.stderr)
    sys.exit(2)

if not os.path.exists(shm_device_path):
    print(f"Shared memory device file {shm_device_path} does not exist.")
    # dd if=/dev/zero of=$devName bs=1M count=1024
    os.system(f"dd if=/dev/zero of={shm_device_path} bs=1G count=16")
    print(f"Shared memory device file {shm_device_path} created.")

# 打开共享内存设备文件
with open(shm_device_path, 'r+b') as shm_fd:
    # 内存映射文件
    shm_file_size = os.path.getsize(shm_device_path)
    shm = mmap.mmap(shm_fd.fileno(), shm_file_size)

    file_num = 0
    offset = PAGE_SIZE
    
    for source_file_path in source_file_list:
        if not os.path.exists(source_file_path):
            print(f"Warning: source file not found, skipping: {source_file_path}")
            continue

        # 打开源文件以读取
        with open(source_file_path, 'rb') as source_file:
            # 获取文件大小
            file_size = os.path.getsize(source_file_path)

            file_offset = offset

            # info
            file_info_list.append({
                "file_offset": offset,
                "file_size": file_size,
                "file_name": source_file_path.split('/')[-1],
            })

            # 写入shm offset的位置
            if file_offset + file_size > shm_file_size:
                print("file_offset + file_size > shm_file_size")
                break

            # write file content to offset
            shm.seek(file_offset)
            source_file.seek(0)

            print("Writing file %s offset: %x size: %x" % (source_file_path, file_offset, file_size))
            shm.write(source_file.read(file_size))

            # 更新offset
            offset += round_up(file_size, PAGE_SIZE)
            file_num += 1
    
    # 写入magic
    shm.seek(0)
    shm.write(b"hostfs\0\0")  # 确保8字节对齐

    # 写入file_num
    shm.seek(8)
    shm.write(struct.pack("<Q", file_num))  # 使用小端序格式化u64

    # 写入file_info_list
    # struct kvm_ivshmem_header {
    #     char magic[8];
    #     u64 file_num;
    #     struct {
    #         u64 file_offset;
    #         u64 file_size;
    #         char file_name[128];
    #     } file_info_list[];
    # } __attribute__((packed, aligned(16)));

    for file_info in file_info_list:
        print(file_info)
    # 计算file_info_list的起始位置
    file_info_list_offset = 16  # magic(8) + file_num(8)
    
    # 遍历file_info_list并写入共享内存
    for i, file_info in enumerate(file_info_list):
        # 计算当前file_info的偏移量
        current_offset = file_info_list_offset + i * (8 + 8 + 128)  # file_offset(8) + file_size(8) + file_name(128)
        
        # 写入file_offset
        shm.seek(current_offset)
        shm.write(struct.pack("<Q", file_info["file_offset"]))
        
        # 写入file_size
        shm.seek(current_offset + 8)
        shm.write(struct.pack("<Q", file_info["file_size"]))
        
        # 写入file_name (确保不超过128字节，不足的部分用0填充)
        shm.seek(current_offset + 16)
        file_name_bytes = file_info["file_name"].encode('utf-8')
        if len(file_name_bytes) > 128:
            file_name_bytes = file_name_bytes[:128]
        else:
            file_name_bytes = file_name_bytes + b'\0' * (128 - len(file_name_bytes))
        shm.write(file_name_bytes)

    # 同步更改回文件系统
    shm.flush()

    # 关闭映射
    shm.close()

print("All files have been copied to shared memory.")
