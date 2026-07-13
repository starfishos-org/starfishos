# Polling IPC Benchmark - 完整资产清单

## 1. 主要日志文件 (含完整CDF数据)

### 📊 polling_latency.log (2.7 MB)
- **内容**: 6个benchmark模式的详细数据
  - empty_t1/t4/t8 (空队列，无文件I/O)
  - read_t1/t4/t8 (读取4KiB文件)
- **包含**: [SUMMARY], [BREAKDOWN_BEGIN/END], [CDF_BEGIN/END]
- **生成方式**: `./dsm-scripts/ipc-test/bench_polling_latency.sh`
- **数据样本**: 1000-8000 samples per mode

**关键数据**:
```
Empty Queue:
  t1: p50= 4.3µs, p99=10.1µs, max=13.1µs
  t4: p50=13.2µs, p99=22.5µs, max=935.3µs
  t8: p50=40.3µs, p99=81.8µs, max=1.56ms

Read 4KiB:
  t1: p50= 7.9µs, p99=14.3µs, max=24.3µs
  t4: p50=25.7µs, p99=33.6µs, max=929.3µs
  t8: p50=57.3µs, p99=68.2µs, max=1.26ms
```

### 📊 exec_log0.log (23 KB)
- **内容**: Direct IPC benchmark (3模式: direct_t1/t4/t8)
- **生成方式**: `./dsm-scripts/ipc-test/bench_direct_ipc.sh`
- **数据样本**: 3000 samples

---

## 2. 绘图脚本 (Python)

### 🎨 plot_3cases.py (4.0 KB) ⭐ 主要图表
```bash
python3 dsm-scripts/ipc-test/plot_3cases.py
```
- **输入**: polling_latency.log
- **输出**: `breakdown_3cases_vertical.png`
- **显示**: 3个竖向堆积柱 (1/4/8 senders)
- **组件**: alloc | enqueue | srv_dequeue | srv_handle | scheduling | wait
- **特点**: legend在右侧

### 🎨 plot_cdf_all.py (2.9 KB)
```bash
python3 dsm-scripts/ipc-test/plot_cdf_all.py
```
- **输入**: polling_latency.log
- **输出**: `polling_cdf_empty.pdf`, `polling_cdf_read.pdf`
- **显示**: CDF曲线 (百分位 vs 延迟)

### 🎨 plot_breakdown_simple.py (4.5 KB)
```bash
python3 dsm-scripts/ipc-test/plot_breakdown_simple.py
```
- **输入**: polling_latency.log
- **输出**:
  - `breakdown_read_comparison.png` (1 vs 4 senders)
  - `breakdown_component_scaling.png` (各组件扩展)

### 🎨 plot_direct_vs_polling.py (3.6 KB)
```bash
python3 dsm-scripts/ipc-test/plot_direct_vs_polling.py
```
- **输入**: polling_latency.log
- **输出**:
  - `ipc_comparison_p50.png` (Direct vs Polling p50对比)
  - `polling_breakdown_detail.png` (水平条形图)

### 🎨 plot_polling_latency.py (4.8 KB)
- **说明**: 旧脚本 (已被plot_cdf_all.py取代)

---

## 3. 生成的PNG图表

### ⭐ 主要图表
- **breakdown_3cases_vertical.png** (82 KB)
  - 3个竖向堆积柱: 1 sender | 4 senders | 8 senders
  - 6个组件颜色清晰区分
  - Legend在右侧

### 📈 对比图表
| 文件 | 大小 | 说明 |
|------|------|------|
| breakdown_read_comparison.png | 66 KB | 1 vs 4 senders对比 |
| breakdown_component_scaling.png | 49 KB | 各组件随并发扩展 |
| ipc_comparison_p50.png | 42 KB | Direct IPC vs Polling IPC |
| polling_breakdown_detail.png | 51 KB | 水平条形图breakdown |
| breakdown_local_1sender.png | 47 KB | Polling 1 sender详细分解 |
| breakdown_local_4senders.png | 48 KB | Polling 4 senders详细分解 |
| breakdown_polling_scaling.png | 50 KB | 1 vs 4 senders (polling only) |

---

## 4. 生成的PDF图表 (CDF曲线)

| 文件 | 大小 | 说明 |
|------|------|------|
| polling_cdf_empty.pdf | 44 KB | Empty queue CDF (t1/t4/t8) |
| polling_cdf_read.pdf | 36 KB | Read 4KiB CDF (t1/t4/t8) |
| polling_cdf.pdf | 11 KB | 综合CDF (旧版本) |

---

## 5. Benchmark脚本

### 🔧 bench_polling_latency.sh
```bash
./dsm-scripts/ipc-test/bench_polling_latency.sh
```
- **运行**: 6个polling benchmark (empty+read, 1/4/8 threads)
- **输出**: polling_latency.log (2.7MB)
- **耗时**: ~120 seconds

### 🔧 bench_direct_ipc.sh
```bash
./dsm-scripts/ipc-test/bench_direct_ipc.sh
```
- **运行**: 3个direct IPC benchmark (1/4/8 threads)
- **输出**: exec_log0.log + direct_ipc.log
- **耗时**: ~90 seconds

---

## 快速使用指南

### 1️⃣ 查看最新数据
```bash
grep "\[SUMMARY\]" polling_latency.log
```

### 2️⃣ 生成所有PNG图表
```bash
python3 dsm-scripts/ipc-test/plot_3cases.py
python3 dsm-scripts/ipc-test/plot_breakdown_simple.py
python3 dsm-scripts/ipc-test/plot_direct_vs_polling.py
```

### 3️⃣ 生成CDF曲线PDF
```bash
python3 dsm-scripts/ipc-test/plot_cdf_all.py
```

### 4️⃣ 重新运行benchmark
```bash
./dsm-scripts/ipc-test/bench_polling_latency.sh
./dsm-scripts/ipc-test/bench_direct_ipc.sh
```

### 5️⃣ 查看详细breakdown统计
```bash
python3 dsm-scripts/ipc-test/analyze_polling.py polling_latency.log
```

---

## 关键发现

### Latency Breakdown (Read 4KiB, Median)

| 组件 | 1 Sender | 4 Senders | 8 Senders | Scaling (8x vs 1x) |
|------|----------|-----------|-----------|-------------------|
| alloc | 0.64µs | 0.64µs | 0.78µs | 1.2x |
| enqueue | 0.70µs | 0.72µs | 1.01µs | 1.4x |
| srv_dequeue | 0.73µs | 0.78µs | 1.04µs | 1.4x |
| srv_handle | 4.60µs | 5.02µs | 5.19µs | 1.1x |
| **scheduling** | **1.19µs** | **18.3µs** | **49.1µs** | **41.2x** ⚠️ |
| wait | 6.52µs | 24.1µs | 55.4µs | 8.5x |
| **TOTAL** | **7.9µs** | **25.7µs** | **57.3µs** | **7.2x** |

### 性能观察
- ✅ Lock-free queue本身性能稳定 (alloc/enqueue/dequeue ~1µs)
- ⚠️ **主要瓶颈**: 内核共享调度队列 (scheduling component 41x增长)
- ⚠️ **等待时间**: 随并发线性扩展 (wait 8.5x)
- ✅ Direct IPC (6µs) vs Polling IPC (7.9µs) 差异仅1.9µs

---

## 文件位置总结

```
/home/wfn/chcore-cxl/
├── polling_latency.log           (2.7 MB, 主要数据)
├── exec_log0.log                 (23 KB, Direct IPC数据)
├── POLLING_BENCHMARK_ASSETS.md   (此文件)
│
├── breakdown_3cases_vertical.png  (82 KB, ⭐主要图表)
├── breakdown_*.png               (6个其他对比图)
├── ipc_comparison_p50.png        (Direct vs Polling)
├── polling_breakdown_detail.png  (详细breakdown)
│
├── polling_cdf_empty.pdf         (CDF曲线)
├── polling_cdf_read.pdf          (CDF曲线)
│
└── dsm-scripts/ipc-test/
    ├── bench_polling_latency.sh  (主benchmark脚本)
    ├── bench_direct_ipc.sh       (Direct IPC脚本)
    ├── plot_3cases.py            (⭐主图表生成脚本)
    ├── plot_cdf_all.py
    ├── plot_breakdown_simple.py
    ├── plot_direct_vs_polling.py
    └── analyze_polling.py        (详细统计分析)
```
