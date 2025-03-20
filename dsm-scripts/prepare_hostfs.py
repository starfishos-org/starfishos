import mmap
import os
import struct

# 要复制到共享内存的文件路径
source_file_list = [
    '/disk/wfn/models/Meta-Llama-3-8B-Instruct.Q5_K_M.gguf',
]

# 共享内存设备文件路径
# get current user name
user_name = os.getenv('USER')
shm_device_path = f'/dev/shm/ivshmem-conn-{user_name}'

# 头部信息，格式为
file_info_list= []

PAGE_SIZE = 4096

def round_up(x, n):
    return ((x) + (n) - 1) & ~((n) - 1)

# 打开共享内存设备文件
with open(shm_device_path, 'r+b') as shm_fd:
    # 内存映射文件
    shm_file_size = os.path.getsize(shm_device_path)
    shm = mmap.mmap(shm_fd.fileno(), shm_file_size)

    file_num = 0
    offset = PAGE_SIZE
    
    for source_file_path in source_file_list:
        # 打开源文件以读取
        with open(source_file_path, 'rb') as source_file:
            # 获取文件大小
            file_size = os.path.getsize(source_file_path)
            
            # 读取文件内容
            # file_content = source_file.read()

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
