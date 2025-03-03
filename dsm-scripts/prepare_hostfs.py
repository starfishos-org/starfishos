import mmap
import os

# 要复制到共享内存的文件路径
source_file_path = '/disk/wfn/models/Meta-Llama-3-8B-Instruct.Q5_K_M.gguf'

# 共享内存设备文件路径
shm_device_path = '/dev/shm/ivshmem-conn-wfn'

# 获取文件大小
file_size = os.path.getsize(source_file_path)

# 打开共享内存设备文件
with open(shm_device_path, 'r+b') as shm_fd:
    # 内存映射文件
    shm = mmap.mmap(shm_fd.fileno(), file_size)

    # 打开源文件以读取
    with open(source_file_path, 'rb') as source_file:
        # 读取文件内容
        file_content = source_file.read()

        # 将文件内容写入共享内存
        shm.write(file_content)

    # 同步更改回文件系统
    shm.flush()

    # 关闭映射
    shm.close()

print("File has been copied to shared memory.")
