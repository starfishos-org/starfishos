● 现在我有了完整的信息来分析。让我梳理清楚：                                                                                        
                                               
  ---                                                                                                                               
  bench_malloc_e2e.sh 测了什么                                                                                                      
                                                                                                                                    
  benchmark 调用参数                                                                                                                
                                                                                                                                    
  脚本固定参数 USER_BENCH_FIXED_ARGS="0 0 0 10 100 1000 16 256" 拼接成：                                                            
                                                                                                                                    
  malloc_benchmark.bin <threads> 0 0 0 10 100 1000 16 256                                                                           
                  
  对应 benchmark_run 参数解析：                                                                                                     
  
  ┌─────────────┬────────┬─────────────────────────────┐                                                                            
  │    参数     │   值   │            含义             │
  ├─────────────┼────────┼─────────────────────────────┤
  │ mode        │ 0      │ MODE_RANDOM                 │
  ├─────────────┼────────┼─────────────────────────────┤
  │ size_mode   │ 0      │ SIZE_MODE_EVEN 均匀分布     │                                                                            
  ├─────────────┼────────┼─────────────────────────────┤
  │ cross_rate  │ 0      │ 不跨线程释放                │                                                                            
  ├─────────────┼────────┼─────────────────────────────┤                                                                            
  │ loop_count  │ 10     │ 10个外循环                  │
  ├─────────────┼────────┼─────────────────────────────┤                                                                            
  │ alloc_count │ 100    │ 每线程同时持有100个指针     │
  ├─────────────┼────────┼─────────────────────────────┤                                                                            
  │ op_count    │ 1000   │ 每loop约1000次alloc/free op │
  ├─────────────┼────────┼─────────────────────────────┤                                                                            
  │ min/max     │ 16/256 │ 随机大小 16~256 字节        │
  └─────────────┴────────┴─────────────────────────────┘                                                                            
                  
  核心工作量：每线程 ~10 loops × 1000 ops，分配 16~256 字节小对象，write第一个int32，然后释放。每线程活跃内存 ≈ 100 × 128B =        
  ~12KB，完全在 L1/L2 cache 内。
                                                                                                                                    
  ---             
  为什么 CXL rpmalloc ≈ DRAM rpmalloc 的根本原因
                                                                                                                                    
  1. rpmalloc 是 thread-local caching allocator
                                                                                                                                    
  rpmalloc 从 OS 申请的是 大 span（默认64KB～64MB）。小对象（16~256B）从 span 的 size-class free-list 直接切割，不回 OS。           
                                                                                                                                    
  关键点：CXL/DRAM 差异只在 mmap → 内核分配物理页时体现，而每次测量的热路径（alloc/free）只操作 span 内的 free-list 指针，这些      
  metadata 已经在 CPU cache 里。
                                                                                                                                    
  2. benchmark.c 实际 malloc 路径                                                                                                   
  
  // benchmark.c:37-43                                                                                                              
  void* ptr = malloc(size);       // rpmalloc: free-list ptr op, O(1), 几条指令
  char dst[size];                                                                                                                   
  memcpy(dst, ptr, size);         // 读 16~256B → 完全在 L1/L2 cache
  *(int32_t*)ptr = (int32_t)size; // 写第一个 int32                                                                                 
                                                                                                                                    
  每次 malloc/free 在 steady-state 只访问：                                                                                         
  - span 的 free-list 链（已在 L1 cache）                                                                                           
  - 分配对象前4字节（存 size）                                                                                                      
                              
  这些操作不 miss cache，因此物理内存在 CXL 还是 DRAM 完全不可见。                                                                  
                                                                                                                                    
  3. mmap 调用的频率极低
                                                                                                                                    
  rpmalloc 对 16~256B 对象按 size class 分 span，一个 span 可以切出几百上千个对象。                                                 
  测试只有 100 个并发 alloc × 10 loops = 极少量 span 申请（可能只有初始化时1~2次 mmap），之后全部走 thread-local cache。
                                                                                                                                    
  CXL 延迟（额外 ~100-300ns/access）只在 mmap → 内核分配物理页时 amortize 掉了。                                                    
                                                                                                                                    
  4. 数据访问集大小 vs cache 容量                                                                                                   
                  
  每线程活跃对象集：100 × avg(128B) = 12.8KB → 完全在 L1D cache（32KB）。                                                           
  就算物理页在 CXL，第一次访问 page fault / cache-miss 之后就常驻 cache，后续操作和内存位置无关。
                                                                                                                                    
  ---             
  总结                                                                                                                              
                  
  ▎ user-space rpmalloc CXL ≈ DRAM 快，是因为这个 benchmark 根本没有压出 CXL 延迟：
  ▎ 1. rpmalloc thread-local cache 掩盖了内存访问延迟（热路径不 miss cache）                                                        
  ▎ 2. 分配对象极小（16~256B），活跃集远小于 L1/L2                                                                                  
  ▎ 3. mmap 次数极少，CXL 延迟被 amortize                                                                                           
  ▎ 4. loop_count=10 + alloc_count=100 的工作量太轻，span 用量少                                                                    
                                                                                                                                    
  要真正观察 CXL 延迟的影响，需要：                                                                                                 
  - 大于 LLC 容量的活跃对象集（LLC 通常 32~64MB，要超过它才会 miss 到 CXL）                                                         
  - stride 访问而不是 cache-friendly 的 free-list 操作                                                                              
  - 或者测 第一次访问延迟（分配后立即随机读整个对象范围）