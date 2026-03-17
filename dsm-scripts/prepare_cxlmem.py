import mmap
import os
import re

# 共享内存设备文件路径
base_dir = "/mnt/cxlshm"
numa_base_dir = "/dev/shm"
user_name = os.getenv('USER')
shm_device_path = f'{base_dir}/ivshmem-{user_name}'

# 8个CXL设备文件路径（numax.x 现在在 /dev/shm）
cxl_devices = [
    f'{numa_base_dir}/numa0.0-{user_name}',
    f'{numa_base_dir}/numa1.0-{user_name}',
    f'{numa_base_dir}/numa2.0-{user_name}',
    f'{numa_base_dir}/numa3.0-{user_name}',
    f'{numa_base_dir}/numa0.1-{user_name}',
    f'{numa_base_dir}/numa1.1-{user_name}',
    f'{numa_base_dir}/numa2.1-{user_name}',
    f'{numa_base_dir}/numa3.1-{user_name}',
]

# 16K
SIZE_16K = 4096 * 4

# 处理共享内存设备文件（cxlmem magic）
if os.path.exists(shm_device_path):
    with open(shm_device_path, 'r+b') as shm_fd:
        # 将前1M设置为0
        shm_fd.seek(0)
        shm_fd.write(b"\0" * SIZE_16K)

        # 写入magic
        shm_fd.seek(0)
        shm_fd.write(b"cxlmem\0\0")  # 确保8字节对齐

        # 同步更改回文件系统
        shm_fd.flush()
        shm_fd.close()
    print(f"Shared memory {shm_device_path} has been prepared.")
else:
    print(f"Shared memory device {shm_device_path} does not exist, skipping.")

# 处理8个CXL设备文件
for dev_path in cxl_devices:
    if not os.path.exists(dev_path):
        print(f"CXL device {dev_path} does not exist, skipping.")
        continue
    
    # 从文件名提取numa信息（例如：numa0.0, numa1.0等）
    filename = os.path.basename(dev_path)
    match = re.search(r'numa(\d+)\.(\d+)', filename)
    if match:
        numa_node = match.group(1)
        numa_dev = match.group(2)
        magic = f"numa{numa_node}.{numa_dev}\0".encode('ascii')
        
        # 确保magic是8字节对齐
        magic_padded = magic.ljust(8, b'\0')
        
        with open(dev_path, 'r+b') as dev_fd:
            # 将前16K设置为0
            dev_fd.seek(0)
            dev_fd.write(b"\0" * SIZE_16K)
            
            # 写入magic
            dev_fd.seek(0)
            dev_fd.write(magic_padded)
            
            # 同步更改回文件系统
            dev_fd.flush()
            dev_fd.close()
        
        print(f"CXL device {dev_path} has been prepared with magic: numa{numa_node}.{numa_dev}")
    else:
        print(f"Warning: Could not extract numa info from {dev_path}")

print("All CXL devices have been prepared.")
