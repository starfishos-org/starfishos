# QEMU/KVM vmexit 开销统计指南

本目录提供了用于统计和分析 QEMU/KVM 运行过程中 vmexit 开销的工具脚本。

## 工具说明

### 1. vmexit_stats.sh - 完整统计脚本

在运行 QEMU 虚拟机时自动记录和分析 vmexit 事件。

**使用方法:**
```bash
sudo ./scripts/qemu/vmexit_stats.sh [vm_id] [command]
```

**示例:**
```bash
# 统计 simulate.sh 运行时的 vmexit
sudo ./scripts/qemu/vmexit_stats.sh 0 './build/simulate.sh 0'
```

**功能:**
- 自动启动 perf kvm stat record 记录 vmexit 事件
- 执行指定的 QEMU 命令
- 自动生成统计报告
- 输出保存到 `/tmp/vmexit_stats_*/` 目录

### 2. vmexit_live.sh - 实时监控

实时显示 vmexit 统计信息，适合在另一个终端监控。

**使用方法:**
```bash
sudo ./scripts/qemu/vmexit_live.sh [interval]
```

**示例:**
```bash
# 每秒刷新一次
sudo ./scripts/qemu/vmexit_live.sh 1

# 每5秒刷新一次
sudo ./scripts/qemu/vmexit_live.sh 5
```

### 3. vmexit_analyze.sh - 分析已记录的数据

分析之前记录的 perf 数据文件。

**使用方法:**
```bash
sudo ./scripts/qemu/vmexit_analyze.sh [perf_data_file]
```

**示例:**
```bash
# 分析默认位置的数据
sudo ./scripts/qemu/vmexit_analyze.sh

# 分析指定文件
sudo ./scripts/qemu/vmexit_analyze.sh /tmp/vmexit_stats_12345/perf.data
```

## 手动方法

### 方法1: 使用 perf kvm stat

**实时统计:**
```bash
sudo perf kvm stat live
```

**记录并分析:**
```bash
# 记录
sudo perf kvm stat record

# 在另一个终端运行 QEMU
./build/simulate.sh 0

# 停止记录 (Ctrl+C)，然后生成报告
sudo perf kvm stat report
```

### 方法2: 使用 perf stat 统计 KVM tracepoints

```bash
# 查看可用的 KVM 事件
sudo perf list | grep kvm

# 统计特定事件
sudo perf stat -e kvm:kvm_exit,kvm:kvm_entry,kvm:kvm_vcpu_wakeup \
    ./build/simulate.sh 0
```

### 方法3: 使用 ftrace

```bash
# 挂载 debugfs (如果未挂载)
sudo mount -t debugfs none /sys/kernel/debug

# 启用 kvm_exit 事件
echo 1 > /sys/kernel/debug/tracing/events/kvm/kvm_exit/enable

# 查看实时事件
cat /sys/kernel/debug/tracing/trace_pipe

# 查看统计
cat /sys/kernel/debug/tracing/events/kvm/kvm_exit/hist
```

### 方法4: 使用 QEMU trace

在 QEMU 启动参数中添加 trace 选项:

```bash
qemu-system-x86_64 \
    -trace events=/path/to/events \
    -trace file=/tmp/qemu_trace.log \
    ...
```

## 常见 vmexit 原因

通过统计可以了解导致 vmexit 的主要原因:

- **EXTERNAL_INTERRUPT**: 外部中断
- **IO_INSTRUCTION**: I/O 指令
- **CPUID**: CPUID 指令
- **MSR_READ/MSR_WRITE**: MSR 读写
- **EPT_VIOLATION**: EPT 页表违规
- **APIC_ACCESS**: APIC 访问
- **HLT**: HLT 指令

## 性能优化建议

1. **减少不必要的 vmexit**:
   - 使用 virtio 设备而非模拟设备
   - 启用 KVM 加速特性 (如 posted interrupts)
   - 优化中断处理

2. **分析 vmexit 开销**:
   - 关注 vmexit 频率高的原因
   - 检查 vmexit 到 vmentry 的延迟
   - 分析不同工作负载下的模式

3. **使用 CPU 特性**:
   - 启用硬件辅助虚拟化特性
   - 使用合适的 CPU 模型 (`-cpu host`)

## 依赖要求

- `perf` 工具 (通常包含在 `linux-perf` 或 `perf` 包中)
- root 权限 (用于访问 KVM tracepoints)
- KVM 模块已加载
- 内核支持 KVM (CONFIG_KVM)

## 故障排除

1. **perf kvm 不可用**:
   - 确保内核支持 KVM
   - 检查 `/sys/module/kvm` 是否存在
   - 某些发行版可能需要安装额外的 perf 包

2. **权限问题**:
   - 使用 sudo 运行脚本
   - 检查 `/proc/sys/kernel/perf_event_paranoid` 设置

3. **没有数据**:
   - 确保 QEMU 使用了 `--enable-kvm` 选项
   - 检查虚拟机是否实际运行在 KVM 模式下

## 参考资料

- [perf kvm 文档](https://www.kernel.org/doc/html/latest/virtual/kvm/tracing.html)
- [KVM 性能调优](https://www.linux-kvm.org/page/Tuning_KVM)
- [QEMU 文档](https://www.qemu.org/documentation/)

