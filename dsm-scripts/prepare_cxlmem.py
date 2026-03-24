import mmap
import os
import re

# 共享内存设备文件路径
base_dir = "/dev/shm"
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


def _get_hugepage_size_bytes() -> int:
    """
    Return system HugeTLB page size in bytes (usually 2MB).
    Falls back to 2MB if parsing fails.
    """
    try:
        with open("/proc/meminfo", "r", encoding="utf-8") as f:
            for line in f:
                if line.startswith("Hugepagesize:"):
                    # Example: "Hugepagesize:       2048 kB"
                    parts = line.split()
                    if len(parts) >= 3 and parts[2].lower() == "kb":
                        return int(parts[1]) * 1024
    except Exception:
        pass
    return 2 * 1024 * 1024


def _is_hugetlbfs_path(path: str) -> bool:
    # Best-effort: in this repo we mount hugetlbfs at /dev/hugepages.
    return os.path.abspath(path).startswith("/dev/hugepages/")


def _write_magic_compatible(dev_path: str, magic_padded: bytes) -> None:
    """
    Write magic to the first bytes and zero the first 16K.

    For hugetlbfs files, plain write() may fail with EINVAL unless hugepage-aligned.
    Use an mmap() of one hugepage and modify it in-memory.
    """
    with open(dev_path, "r+b") as dev_fd:
        if _is_hugetlbfs_path(dev_path):
            hpsz = _get_hugepage_size_bytes()
            # Map at least one hugepage so we can touch the first bytes safely.
            mm = mmap.mmap(dev_fd.fileno(), hpsz, access=mmap.ACCESS_WRITE)
            mm[0:SIZE_16K] = b"\0" * SIZE_16K
            mm[0:len(magic_padded)] = magic_padded
            mm.flush()
            mm.close()
        else:
            # Regular files (e.g., tmpfs /dev/shm) support small writes.
            dev_fd.seek(0)
            dev_fd.write(b"\0" * SIZE_16K)
            dev_fd.seek(0)
            dev_fd.write(magic_padded)
            dev_fd.flush()

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

        _write_magic_compatible(dev_path, magic_padded)
        
        print(f"CXL device {dev_path} has been prepared with magic: numa{numa_node}.{numa_dev}")
    else:
        print(f"Warning: Could not extract numa info from {dev_path}")

print("All CXL devices have been prepared.")
