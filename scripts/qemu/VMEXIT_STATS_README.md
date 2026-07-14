# QEMU/KVM vmexit Overhead Statistics Guide

This directory provides tool scripts for collecting and analyzing vmexit overhead while QEMU/KVM is running.

## Tools

### 1. vmexit_stats.sh — full statistics script

Automatically records and analyzes vmexit events while a QEMU VM is running.

**Usage:**
```bash
sudo ./scripts/qemu/vmexit_stats.sh [vm_id] [command]
```

**Example:**
```bash
# Collect vmexit stats while simulate.sh is running
sudo ./scripts/qemu/vmexit_stats.sh 0 './build/simulate.sh 0'
```

**Features:**
- Automatically starts `perf kvm stat record` to capture vmexit events
- Runs the specified QEMU command
- Automatically generates a statistics report
- Saves output under `/tmp/vmexit_stats_*/`

### 2. vmexit_live.sh — live monitoring

Displays vmexit statistics in real time; suitable for monitoring from another terminal.

**Usage:**
```bash
sudo ./scripts/qemu/vmexit_live.sh [interval]
```

**Example:**
```bash
# Refresh once per second
sudo ./scripts/qemu/vmexit_live.sh 1

# Refresh every 5 seconds
sudo ./scripts/qemu/vmexit_live.sh 5
```

### 3. vmexit_analyze.sh — analyze recorded data

Analyzes previously recorded perf data files.

**Usage:**
```bash
sudo ./scripts/qemu/vmexit_analyze.sh [perf_data_file]
```

**Example:**
```bash
# Analyze data at the default location
sudo ./scripts/qemu/vmexit_analyze.sh

# Analyze a specific file
sudo ./scripts/qemu/vmexit_analyze.sh /tmp/vmexit_stats_12345/perf.data
```

## Manual methods

### Method 1: Use perf kvm stat

**Live statistics:**
```bash
sudo perf kvm stat live
```

**Record and analyze:**
```bash
# Record
sudo perf kvm stat record

# In another terminal, run QEMU
./build/simulate.sh 0

# Stop recording (Ctrl+C), then generate the report
sudo perf kvm stat report
```

### Method 2: Use perf stat on KVM tracepoints

```bash
# List available KVM events
sudo perf list | grep kvm

# Count specific events
sudo perf stat -e kvm:kvm_exit,kvm:kvm_entry,kvm:kvm_vcpu_wakeup \
    ./build/simulate.sh 0
```

### Method 3: Use ftrace

```bash
# Mount debugfs (if not already mounted)
sudo mount -t debugfs none /sys/kernel/debug

# Enable the kvm_exit event
echo 1 > /sys/kernel/debug/tracing/events/kvm/kvm_exit/enable

# View live events
cat /sys/kernel/debug/tracing/trace_pipe

# View statistics
cat /sys/kernel/debug/tracing/events/kvm/kvm_exit/hist
```

### Method 4: Use QEMU trace

Add trace options to the QEMU launch command:

```bash
qemu-system-x86_64 \
    -trace events=/path/to/events \
    -trace file=/tmp/qemu_trace.log \
    ...
```

## Common vmexit causes

Statistics help identify the main causes of vmexit:

- **EXTERNAL_INTERRUPT**: external interrupt
- **IO_INSTRUCTION**: I/O instruction
- **CPUID**: CPUID instruction
- **MSR_READ/MSR_WRITE**: MSR read/write
- **EPT_VIOLATION**: EPT page-table violation
- **APIC_ACCESS**: APIC access
- **HLT**: HLT instruction

## Performance tuning tips

1. **Reduce unnecessary vmexits**:
   - Prefer virtio devices over emulated devices
   - Enable KVM acceleration features (e.g. posted interrupts)
   - Optimize interrupt handling

2. **Analyze vmexit cost**:
   - Focus on high-frequency vmexit reasons
   - Measure latency from vmexit to vmentry
   - Compare patterns under different workloads

3. **Use CPU features**:
   - Enable hardware-assisted virtualization features
   - Use an appropriate CPU model (`-cpu host`)

## Dependencies

- `perf` tool (usually from the `linux-perf` or `perf` package)
- Root privileges (to access KVM tracepoints)
- KVM module loaded
- Kernel with KVM support (`CONFIG_KVM`)

## Troubleshooting

1. **perf kvm unavailable**:
   - Ensure the kernel supports KVM
   - Check that `/sys/module/kvm` exists
   - Some distributions may need an extra perf package

2. **Permission issues**:
   - Run the scripts with sudo
   - Check `/proc/sys/kernel/perf_event_paranoid`

3. **No data**:
   - Ensure QEMU is started with `--enable-kvm`
   - Verify the VM is actually running under KVM

## References

- [perf kvm documentation](https://www.kernel.org/doc/html/latest/virtual/kvm/tracing.html)
- [KVM performance tuning](https://www.linux-kvm.org/page/Tuning_KVM)
- [QEMU documentation](https://www.qemu.org/documentation/)
