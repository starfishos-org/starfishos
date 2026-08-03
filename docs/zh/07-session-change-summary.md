# 故障恢复与服务重连设计

本文说明 machine rejoin、跨机器任务故障、服务请求中止、文件系统实例替换以及客户端重连机制。

## 机器身份与 generation

每台逻辑机器拥有稳定的 machine ID，以及单调递增的 `boot_generation`。机器重新加入 cluster 时复用原 machine ID，但使用新的 generation。CXL 中的元数据通过 `(machine_id, boot_generation)` 区分当前 incarnation 和旧 incarnation 发布的指针、任务、capability 以及请求。

DRAM 中的易失性结构体在 boot 阶段重新建立；包含 cluster 协调状态、持久队列节点、文件系统元数据和 p-log 映射的 CXL 结构体在 rejoin 时复用。

## 跨机器任务故障

跨机器任务记录每个 participant 的 machine ID 和 boot generation。存活的 owner 发现 participant 的 generation 发生变化后，将该 participant 标记为故障，终止其执行状态，并回收对应的分布式 capability 和其他资源。owner 任务继续存活，故障 participant 从 active membership 中移除。

恢复路径必须区分“participant 故障”和“整个任务故障”。因此清理范围只针对故障 participant 的资源，并且清理操作需要支持重复执行。

## 持久服务请求

持久服务请求使用以下状态机：

```text
FREE -> INIT -> DOING -> DONE
                   \\-> ABORT
```

`INIT` 表示请求已经发布但还没有被服务端领取，替代服务可以继续执行。`DOING` 的结果不确定，因为旧服务可能已经部分执行；恢复时将其转换成 `ABORT`。`DONE` 包含有效响应，继续保留给客户端。

客户端收到 `ABORT` 后得到 `-ECONNABORTED`。传输层不会自动重放结果不确定的写操作；由应用或具体服务的恢复协议决定重试、撤销还是报告失败。

## 全局文件系统实例注册表

每个持久文件系统 shard 在 CXL 中拥有一条全局路由记录：

```text
shard_id -> {
    server_thread,
    instance_generation,
    host_machine_id,
    host_boot_generation,
    state
}
```

`shard_id` 标识持久文件系统数据；`host_machine_id` 和 `host_boot_generation` 标识当前服务所在的机器。每个替代 FS 服务注册时递增 `instance_generation`，并原子发布新的 server endpoint。

客户端在使用 FS IPC connection 前比较注册表中的 generation。generation 不一致时，旧 connection 失效，客户端向新的服务端重新注册 connection。

## 文件描述符重连

为了重建服务端 session，应用侧文件描述符需要保存：

- 持久 shard ID；
- 服务端 pathname；
- 原始 open flags 和 mode；
- 当前文件 offset；
- 打开该文件时使用的 FS instance generation。

generation 变化后，客户端重新连接 FS 服务，使用原 pathname 重新打开文件，但不携带 `O_CREAT`、`O_EXCL` 或 `O_TRUNC`，随后恢复 offset，并继续使用原来的应用侧 fd。这样 LevelDB 等长期持有文件的应用可以连接到替代 FS instance。

旧服务故障时已经在执行的操作不会被自动重放，因为其结果不确定。该操作必须作为 I/O error 返回，或者交由应用的 WAL/恢复协议处理。重连机制只保证后续操作。

## LevelDB 恢复模型

LevelDB 将失败的 POSIX 文件操作转换为 I/O error。安全的恢复顺序是：

1. 报告被中断的操作失败；
2. 关闭或丢弃受影响的 LevelDB 实例和旧文件 session；
3. 等待替代 FS instance 发布新的 generation；
4. 重新打开数据库；
5. 使用 WAL 和 manifest 恢复已经提交的状态。

传输层不会透明重放被中断的 WAL write，因为旧服务失败前该 write 可能已经到达持久存储。

## 故障检测与立即 abort

目标故障处理时序应为：

```text
故障检测器 -> 将机器/服务标记为 OFFLINE
           -> fence 故障 generation 的 CXL/cache agent
           -> 立即 abort 相关请求和 IPC call
           -> 把错误返回给上层应用
           -> 替代服务 rejoin 并发布新的 generation
           -> 应用重连并决定是否重试
```

`ABORT` 应该在存活机器检测到故障时发布，而不能等到替代服务最终启动后才发布。heartbeat 或等价的故障检测器需要把机器/服务注册表转换为 `OFFLINE`，并找出属于故障 generation 的所有 connection、队列请求和跨机器任务。kernel 需要唤醒阻塞在远程 IPC 中的客户端，并返回明确的 abort 错误。替代服务启动后再负责持久队列恢复和 generation 发布，但不应该成为客户端第一次得知故障的时刻。

当前实现已经具备 generation 和 rejoin 记录，但服务请求的 `ABORT` 仍然是在替代服务启动时发布。要实现立即把故障交给应用处理，还需要把 abort 发布逻辑前移到故障检测路径。

### Cache coherence、故障 fencing 与持久性边界

CXL cache coherence、原子可见性和故障后的持久性是三个不同保证。只要存活机器
B 的 CAS 已经成功且 B 仍在线，其他仍在线的 coherent participant 应当能够观察到
该 CAS 的结果；这个可见性由 coherence protocol 保证，不依赖 `clwb`。但是，如果
结果只存在于 B 的 dirty cache 中，B 随后也发生故障，该结果仍可能丢失。需要在
CAS 后执行 `clwb` 和 `sfence`，才能把结果发布到单 host failure 后仍存活的
persistence domain。

相反，故障机器 A cache 中尚未持久化的修改不能由 coherence protocol 恢复。例如，
CXL media 中的状态仍是 `INIT`，而 A 的 dirty cache 中是 `DOING` 时，A 故障可能使
恢复者重新看到 `INIT`。因此 server 必须在产生任何请求副作用之前持久化 `DOING`：

```text
CAS(INIT -> DOING)
clwb(&status)
sfence
handle(request)
```

恢复不能与旧 cache agent 并发进行。failure detector 在扫描 queue 之前，必须先
fence 故障的 `(machine_id, boot_generation)`，使旧实例不能继续访问 CXL，并确认
旧实例的 outstanding transactions 不会在恢复写入之后晚到。随后存活机器才能执行
`DOING -> ABORT` 的 CAS，并用 `clwb + sfence` 持久化结果。若 platform 无法清理
故障 cache agent 的 directory ownership，或者访问返回 poison/error，则不能把一次
成功的软件恢复当作既定前提；相关 queue 必须保持隔离，直到 fabric/platform 完成
恢复。

因此本文依赖的故障边界是：故障 host 的 CPU cache 可以丢失，但 CXL memory、fabric
和 coherence home agent 继续工作，并支持对故障 host 进行 fencing。`clwb + sfence`
只保证当前写入机器随后故障时结果仍存在，不能在故障发生后找回另一台机器已经丢失
的 dirty cache line。

## QEMU host 层 failure detector

`dsm-scripts/host-failure-detector.sh` 是 host 侧存活检测器。它定期用
`kill -0` 检查 QEMU 进程；进程退出后，在事件日志中追加持久化的
`machine_failure` 记录和兼容现有测试的 `machine<ID>_qemu_exited` 标记。传入
`--notify-pid` 时，还会向 host 控制进程发送 `SIGUSR1`；控制进程随后负责将机器
标记为 offline，并发布受影响请求和 cross-machine task 的 abort。这样 QEMU 进程
检测与 guest IPC 投递解耦，也能覆盖 rejoin 后的新实例。传入
`--restart-cmd` 后，同一个控制器会启动替代 QEMU 并继续监控新的 PID。需要
主动停止整个 OS 时，应先创建配置的 `--stop-file`（或先终止 detector），这样
正常关机不会被误判为故障并自动重启。
