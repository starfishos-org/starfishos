import mmap
import os

# 共享内存设备文件路径
user_name = os.getenv('USER')
shm_device_path = f'/dev/shm/ivshmem-{user_name}'

# 4K
SIZE_4K = 4096

# 打开共享内存设备文件
if not os.path.exists(shm_device_path):
    print(f"Shared memory device {shm_device_path} does not exist.")
    exit(1)

with open(shm_device_path, 'r+b') as shm_fd:
    # 将前1M设置为0
    shm_fd.seek(0)
    shm_fd.write(b"\0" * SIZE_4K)

    # 写入magic
    shm_fd.seek(0)
    shm_fd.write(b"cxlmem\0\0")  # 确保8字节对齐

    # 同步更改回文件系统
    shm_fd.flush()

    # 关闭映射
    shm_fd.close()

print("Shared memory has been prepared.")
