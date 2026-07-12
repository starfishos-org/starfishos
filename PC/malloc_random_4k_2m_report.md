# 内核随机 malloc/free（4KiB / 2MiB）测试报告

**生成时间**：2026-03-28  
**环境**：本地 `chbuild build` 产物，`build/simulate.sh 0`，`MACHINE_NUM=1`，`CPU_NUM=4`，`USE_DEV_AS_DRAM=1`，日志来源 `build/exec_log0.log`。

## 1. 测试说明

- **实现位置**：`kernel/tests/tst_malloc.c` 中 `run_random_kmalloc_free_4k_2m()`，由 `tst_malloc()` 在既有 kmalloc / get_pages 用例之后调用。
- **单次迭代**：`rng & 3` 四选一（各 **25%** 意图）  
  - 0：`kmalloc(4096)`  
  - 1：`kmalloc(2MiB)`  
  - 2：若存在已分配 4KiB 块则 `kfree`，否则改为 `kmalloc(4096)`（记 `fb_free_empty_4k`）  
  - 3：若存在已分配 2MiB 块则 `kfree`，否则改为 `kmalloc(2MiB)`（记 `fb_free_empty_2m`）  
- **迭代次数**：`tst_random_malloc_free_iterations = 20000`（每轮 DRAM / CXL 各 20000 步）。  
- **执行 CPU**：仅 CPU0 执行随机循环；其它 CPU 参与 barrier（与 `parallel` 一致）。  
- **本轮有效数据**：仅 **`parallel=1`** 完整结束；随后开始 `parallel=2` 时被 `timeout` 终止，故 **未包含** 更高并行度下的完整结果。

## 2. 原始日志摘录

```
[INFO] [TEST] start malloc test parallel=1
[INFO] [TEST] DRAM random malloc/free 4KiB+2MiB iters=20000 malloc_4k=4964 malloc_2m=5113 free_4k=4904 free_2m=5019 fb_free_empty_4k=20 fb_free_empty_2m=20 (op pick 25% each; fb = free chosen but pool empty)
[INFO] [TEST] CXL random malloc/free 4KiB+2MiB iters=20000 malloc_4k=5072 malloc_2m=4954 free_4k=5033 free_2m=4941 fb_free_empty_4k=50 fb_free_empty_2m=123 (op pick 25% each; fb = free chosen but pool empty)
[INFO] [TEST] malloc succ!
[INFO] [TEST] start malloc test parallel=2
…（此后 QEMU 被 timeout 中断）
```

## 3. 计数校验

每步**恰好**执行一类「记账事件」，故应满足：

`malloc_4k + malloc_2m + free_4k + free_2m = iters`

| 内存类型 | 求和 | 是否等于 20000 |
|----------|------|----------------|
| DRAM     | 4964+5113+4904+5019 | ✓ |
| CXL      | 5072+4954+5033+4941 | ✓ |

## 4. 结果汇总表（`parallel=1`）

| 指标 | DRAM | CXL |
|------|------|-----|
| `malloc_4k` | 4964 (24.82%) | 5072 (25.36%) |
| `malloc_2m` | 5113 (25.57%) | 4954 (24.77%) |
| `free_4k` | 4904 (24.52%) | 5033 (25.17%) |
| `free_2m` | 5019 (25.10%) | 4941 (24.71%) |
| `fb_free_empty_4k` | 20 | 50 |
| `fb_free_empty_2m` | 20 | 123 |

（百分比为相对 `iters=20000`。）

## 5. 简要分析

1. **四类事件占比**  
   在 20000 步下，四类计数均在 **24.5%～25.6%** 区间，与「每步均匀四选一」的预期一致；微小偏差来自随机波动与有限步数。

2. **fallback（`fb_*`）**  
   - 表示「抽到 free 但对应尺寸池为空」的次数，此时会改为同尺寸 `malloc`，**不会**对未分配指针 `kfree`。  
   - DRAM：`fb` 合计 40（0.20%）。  
   - CXL：`fb` 合计 173（0.87%），其中 **2MiB 侧 fallback 更多**（123），符合大对象池更易在随机游走下暂时为空的直觉。  

3. **隐含「操作码」次数（可选核对）**  
   - 抽到 op2 的次数 ≈ `free_4k + fb_free_empty_4k`（DRAM 4924，CXL 5083）。  
   - 抽到 op3 的次数 ≈ `free_2m + fb_free_empty_2m`（DRAM 5039，CXL 5064）。  
   - 抽到 op0 的次数 ≈ `malloc_4k - fb_free_empty_4k`（DRAM 4944，CXL 5022）。  
   - 抽到 op1 的次数 ≈ `malloc_2m - fb_free_empty_2m`（DRAM 5093，CXL 4831）。  
   四者之和均为 20000。

4. **本轮限制**  
   未跑完 `parallel=2` 及更高并行度；若需多核下的随机 malloc 统计，应延长运行时间或暂时收窄 `parallel_levels` 仅测目标并行度。

## 6. 复现命令（参考）

```bash
cd build
rm -f exec_log0.log
# 适当增大 timeout；CPU_NUM 可按机器调整
timeout 600 env MACHINE_NUM=1 CPU_NUM=4 USE_DEV_AS_DRAM=1 ./simulate.sh 0
grep 'random malloc/free' exec_log0.log
```

---

**结论**：在 `parallel=1`、每轮 20000 次迭代下，DRAM 与 CXL 路径上 **4KiB/2MiB 的 malloc 与 free 计数均接近各 25%**；`fb_*` 很少，CXL 上 2MiB 的 fallback 略多但仍属低比例。测试逻辑与计数自洽（四类之和等于 `iters`）。
