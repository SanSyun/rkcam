# DMA 缓存一致性问题与 dma-buf 显式同步

> 工程：`mediaCam` 主线（RK3568 + IMX415 + RGA + MPP）
> 涉及文件：`src/u_dma.c`、`src/thread_capture.c`、`src/thread_encode.c`、`src/v4l2_capture.c`
> 目标：讲清"为什么 CPU 和 DMA 会看到不同的数据"、本工程用哪条路线解决、现有实现的隐患、以及零拷贝改造后问题为何消失
> 记录时间：2026-11-01

---

## 一、问题是什么

### 1.1 一句话描述

RK3568 的 Cortex-A55 读写内存要经过 L1/L2/共享 L3，而 RGA、VPU（MPP 编码器）、ISP/vicap 这些 DMA 引擎**绕过 CPU cache 直接读写 DDR**。同一块物理内存被"走 cache 的 CPU"和"不走 cache 的设备"同时访问，双方看到的数据就可能不一致。

这不是 bug，是体系结构的必然结果。软件必须显式告诉内核"现在谁在访问这块内存"，否则就是数据竞争。

### 1.2 三种失败模式

| 模式 | 现象 | 成因 |
| --- | --- | --- |
| **写丢失** | 设备读到旧图像 / 编码出来是上一帧 | CPU 的 `memcpy` 结果还在 cache 脏行里，没落到 DDR |
| **读陈旧** | CPU 读到上一帧内容 | 设备写了 DDR，但 CPU cache 里还是旧行，命中即返回旧值 |
| **回写破坏**（最阴） | 偶发花屏、半帧错位、跑几分钟才出一次 | CPU 的脏行延迟回写，把设备刚写好的新数据盖掉了 |

第三种最难查：它不在 `memcpy` 那一刻发生，而在后面某时刻 cache 行被替换时才爆，所以表现为"偶发、随负载变化、重启又好了"。**这正是必须严格成对调用 begin/end、而不能靠"我 memcpy 完立刻就编码了，应该没事"的原因。**

### 1.3 为什么 RK3568 上必然存在

三个硬件事实决定了这件事躲不掉：

1. **外设不挂一致性总线**。ARM 平台的一致性靠 CCI/CMN 这类互连给设备提供 snoop 能力（DT 里的 `dma-coherent` 属性）。RK3568 的 RGA2、VPU、ISP、VOP 都不挂一致性端口，DMA 读写不会探测 CPU cache。
2. **IOMMU ≠ cache 一致性**。RK3568 有 IOMMU，但它只做地址翻译和访问隔离，不做 snoop。这是最常见的混淆点。
3. **默认的 dma-buf 分配是 non-coherent**。`/dev/dma_heap/system` 用 `alloc_pages` + `dma_map_sgtable`，属于 **streaming DMA 映射**：CPU 和设备的访问顺序由软件保证。

---

## 二、四条解决路线

| 路线 | 做法 | 代价 | RK3568 可用性 |
| --- | --- | --- | --- |
| **A. 一致性内存** | `dma_alloc_coherent()`，硬件/映射层面保证 | CPU 访问慢（uncached/WC），3 MB 帧的 memcpy 会慢 5–10 倍 | 驱动内部用，用户态拿不到 |
| **B. 流式 DMA + 显式同步** | non-coherent 内存，传输前后 `dma_sync_sg_for_cpu/device`；用户态对应 `DMA_BUF_IOCTL_SYNC` | 多两次同步（几十 µs 级）+ 编程纪律 | ✅ **本工程走的这条** |
| **C. 硬件 snoop** | DT 加 `dma-coherent`，设备挂一致性端口 | 无软件开销 | ❌ RK 的 RGA/VPU/ISP 都不支持 |
| **D. 避免共享** | 数据只在设备之间流动，CPU 完全不碰 | 需要 buffer 池 + fd 传递 + 引用计数 | ✅ 真正的零拷贝，见第五节 |

### 关于 dma-heap 的几个 heap

`/dev/dma_heap/` 下几个节点的语义完全不同，别混用：

| heap | 一致性模型 | 需要显式 sync | 适用场景 |
| --- | --- | --- | --- |
| `system` | non-coherent streaming | **是** | 本工程（CPU 要整帧读写） |
| `cma` | non-coherent streaming | **是** | 大块连续物理内存 |
| `system-uncached` | 映射成 uncached | 否 | CPU 只碰元数据，或纯设备间搬运 |

有人图省事换 `system-uncached` 来"消除一致性问题"，结果 1080p 的 memcpy 直接掉到个位数帧率——因为每次访问都变成总线事务。**对"CPU 要整帧读写"的场景，B 是正解，不要用 uncached 偷懒。**

---

## 三、本工程的实现

### 3.1 buffer 从哪来

两条来源，最终都落在 dma-buf 上：

| 用途 | 分配方式 | 拿 fd | 拿 CPU 地址 |
| --- | --- | --- | --- |
| 采集原始帧 | V4L2 `REQBUFS(MMAP)` | `VIDIOC_EXPBUF` → `buffers[i].fd` | `mmap` → `buffers[i].start` |
| RGA 输出 / MPP 输入 | `mpp_buffer_get()` | `mpp_buffer_get_fd()` | `mpp_buffer_get_ptr()` |

关键点：`mmap` 和 `EXPBUF` 指向**同一块物理内存**的两个视图。前者给 CPU，后者给设备。`mpp_buffer_get()` 底层同样走 dma-heap，所以 `DMA_BUF_IOCTL_SYNC` 能打到它的 `begin_cpu_access` / `end_cpu_access` 回调上。

> 想确认实际来源：`cat /sys/kernel/debug/dma_buf/bufinfo`，看 exporter 是不是 `system`。只有 non-coherent 的来源才真的需要同步。

### 3.2 数据流与同步边界

```text
IMX415 ──DMA写──► V4L2 MMAP buffer (dma-buf)
                       │
                       │ ① vb2 在 QBUF/DQBUF 边界自动 sync，本工程无需手写
                       │    （CPU 其实不读这块，直接把 fd 交给 RGA）
                       ▼
                    RGA2 (DMA 读写，设备→设备，不经过 CPU cache)
                       │
                       ▼
                 rga_buf (dma-heap system, non-coherent)
                       │
                       │ ② dma_buf_cpu_begin(fd, 0)  ← CPU 要读
                       ▼
                 CPU memcpy → frame_rb（普通 malloc 内存）
                       │
                       │ ③ 纯 CPU 内存，无一致性问题
                       ▼
                 CPU memcpy → enc_buf (dma-heap system, non-coherent)
                       │
                       │ ④ dma_buf_cpu_end(fd, 1)    → clean
                       ▼
                 MPP 编码器 (DMA 读)
```

只有 ② 和 ④ 两个边界需要手写同步，因为它们**跨过了 CPU**。①③ 不需要：

- ① 由 V4L2 的 videobuf2 框架负责；
- ③ 是普通内存，设备根本不碰。

一句话记忆：**begin/end 不是在"保护 buffer"，是在标注"接下来这段时间 cache 里可能有假数据，谁也别信"。**

### 3.3 方向语义（最容易搞反的地方）

`DMA_BUF_IOCTL_SYNC` 的 `flags` 描述的是 **CPU 接下来要干什么**，不是"设备干了什么"，更不是"谁拥有这块内存"。

| 场景 | 代码位置 | 调用 | 内核实际动作 |
| --- | --- | --- | --- |
| RGA 写完 → CPU 读 | `thread_capture.c` | `begin(fd, 0)` = `START\|READ` | **invalidate**：作废 cache 旧行，强制从 DDR 重读 |
| | | `end(fd, 0)` = `END\|READ` | 同样是 invalidate（无脏行时等价空转，无害） |
| CPU 写 → MPP 读 | `thread_encode.c` | `begin(fd, 1)` = `START\|WRITE` | **空操作**（见 3.5） |
| | | `end(fd, 1)` = `END\|WRITE` | **clean**：把脏行刷到 PoC，设备才读得到 |

方向的映射关系：`SYNC_READ → DMA_FROM_DEVICE`、`SYNC_WRITE → DMA_TO_DEVICE`、`SYNC_RW → DMA_BIDIRECTIONAL`。

本工程两处**都写对了**：采集侧 `write_access=0`（CPU 只读），编码侧 `write_access=1`（CPU 写）。绝大多数 bug 出在有人把这两处写反——采集侧写成 1 只是多一次无谓的 clean；编码侧写成 0 就会直接踩中 1.2 的"写丢失"，编码器读到的是上一帧的残影。

### 3.4 为什么 invalidate 必须配套 clean

ARM64 上 `dc ivac`（invalidate by VA）作用在**脏行**上属于未定义行为，可能直接丢掉 CPU 尚未回写的数据。所以协议是：

```text
设备写 → CPU 读：  invalidate(丢弃旧行) ── CPU 读 ── 完成
CPU 写 → 设备读：  CPU 写 ── clean(刷脏行) ── 设备读
```

`SYNC_END | WRITE` 的那次 clean 绝不能省。反向也一样：任何"设备将要写"的区域，在设备动手前必须保证 cache 里没有 CPU 的脏行，否则脏行会在未来某刻回写、覆盖设备结果——这就是 1.2 的第三种失败模式。

### 3.5 一个常见误解：`begin(WRITE)` 并不 invalidate

很多资料（包括直觉）会说"写之前要先 invalidate 一次，清掉旧脏行"。但看 arm64 的实际实现：

```c
arch_sync_dma_for_cpu(paddr, size, DMA_TO_DEVICE)      -> 空操作
arch_sync_dma_for_cpu(paddr, size, DMA_FROM_DEVICE)    -> dcache_inval_poc()
arch_sync_dma_for_device(paddr, size, DMA_FROM_DEVICE) -> dcache_inval_poc()
arch_sync_dma_for_device(paddr, size, DMA_TO_DEVICE)   -> dcache_clean_poc()
```

所以 `begin(WRITE)` 在 arm64 上是空转，真正起作用的是 `end(WRITE)` 的 clean。为什么这样也安全？因为 CPU 是**整块重写**：旧行会被写操作就地覆盖（write-allocate），不存在"残留脏行被后续 clean 刷出去污染设备"的路径；而 clean 只刷本次变脏的行，未写的区域不会被写回。

**但不要因此省略 `begin`**：它是语义上的"事务边界"，在双缓冲/多线程以后会成为你唯一能标注"这块内存现在归 CPU"的位置；而且在 `SYNC_RW` 或部分写入的场景下，`begin` 就可能真的做 invalidate。

### 3.6 V4L2 侧为什么不用手写同步

采集 buffer 是 `V4L2_MEMORY_MMAP` 分配的，videobuf2 会在 `QBUF`/`DQBUF` 边界自动做 `dma_sync_sg_for_cpu/device`：

- 摄像头 DMA 写完 → `DQBUF` 时框架已 invalidate，CPU 通过 `buffers[i].start` 读到的一定是新帧；
- CPU（如果有）读完之后 → `QBUF` 时框架会处理脏行。

本工程的 `thread_capture` 恰好没有走 mmap 读取，而是把 `EXPBUF` 的 fd 直接交给 `wrapbuffer_fd()` 给 RGA。RGA 是设备读，不经过 CPU cache，所以这一段同样不需要手动同步。

---

## 四、现有实现的隐患

### 4.1 `ENOTTY/EINVAL` 静默降级（最该改的一处）

`src/u_dma.c` 里：

```c
ret = ioctl(fd, DMA_BUF_IOCTL_SYNC, &sync);
if (ret < 0) {
    if (errno == ENOTTY || errno == EINVAL) {
        return 0;   /* 该内核/驱动不支持显式同步 */
    }
    return -1;
}
```

降级的意图是好的（不想在不支持的内核上直接失败），但它把"一致性保护失效"变成了"静默成功"。更麻烦的是这两个 errno 的真实含义：

| errno | 实际原因 |
| --- | --- |
| `ENOTTY` | **这个 fd 根本不是 dma-buf**（比如误传了 video 设备 fd、普通文件 fd） |
| `EINVAL` | flags 组合非法（方向写错、缺 `START`/`END`） |

也就是说，这个分支吞掉的很可能不是"内核不支持"，而是**调用方参数用错了**。后果是：一旦哪天 MPP 换了 buffer 后端（回到 ION，或换 cma heap 而驱动没实现回调），同步全部变成空操作，然后开始"偶发花屏、跑十分钟才出一次"。

建议：至少做一次性的能力探测并打印 warning，例如在流水线初始化阶段对 `rga_buf`/`enc_buf` 各试一次 sync，确认真的返回 0；而不是每帧都静默吞掉。

### 4.2 CPU 拷贝的带宽账

每帧 NV12 = **3,110,400 B**。当前链路有**两次**全量拷贝：

```text
rga_buf ──copy1──► frame_rb ──copy2──► enc_buf
```

每次拷贝都是"读 + 写"双向流量，所以单帧内存流量是：

```text
2 次拷贝 × (3.11 MB 读 + 3.11 MB 写) = 12.4 MB / 帧
30 fps -> 373 MB/s
25 fps -> 311 MB/s
```

> 补充：`三线程采集编码消费流水线改造.md` 里写的"两次拷贝约 155 MB/s"只计了**写侧**，读侧的带宽开销是同等量级的，双向合计要翻倍。

RK3568 的 LPDDR4x 带宽在 10 GB/s 量级，373 MB/s 占比不到 4%，不构成吞吐瓶颈；但它会**持续污染 L3**，把采集/编码线程的有效工作集挤出去。如果哪天实测有多余耗时，优先怀疑这里，而不是同步 ioctl 本身（单次 sync 是几十 µs 量级）。

### 4.3 sync 窗口开得太长

`thread_encode` 在 `pipeline_read_frame` **之前**就 `begin` 了，而这个读最长阻塞 100 ms。协议上要求"CPU 访问窗口内不能把 buffer 交给设备"。现在只有 MPP 一个设备、且顺序执行，所以安全；但意味着这块 dma-buf 有 100 ms 处于"不可用于 DMA"的状态。

未来如果把 MPP 输入改成双缓冲、或让 RGA 与 MPP 并行，这个顺序必须调整成**一帧一开一关、紧贴 memcpy**。

### 4.4 cache line 对齐

ARM64 cache line 是 64 字节。本工程用 ioctl 而非 `cacheflush` syscall，内核按 sg_table 的页粒度处理，**不会踩"部分行失效误伤邻居"的坑**——这点做对了。

但要记住：任何手写的 `dc civac` / `__builtin___clear_cache` 方案，只要 buffer 起止地址不是 64 字节对齐，就会把相邻数据一起废掉。这是另一个经典事故现场，不要绕开 ioctl 自己刷。

### 4.5 其他注意点

1. **多 plane**：NV12 是单 plane，一个 fd 一次 sync 搞定。若改用 NV12M 双 plane 且拆成两个 dma-buf，则每个 fd 都要 sync。
2. **buffer 复用顺序**：必须保证 `end(WRITE)` 的 clean 完成后，才 `encode_put_frame()`；反过来，必须保证当前帧的 packet 已全部取出，才覆盖 `enc_buf`。本工程两处顺序都是对的。
3. **同步失败要当致命错误**：`dma_buf_cpu_begin/end` 返回 -1 时置 `fatal` 是合理策略——继续跑下去只会产生损坏的码流。

---

## 五、零拷贝方向：让问题消失

如果环形缓冲区从"传像素"改成"传 fd + 引用计数"，CPU 一次都不读像素：

```text
capture: RGA 输出 dma-buf ──写进环 {fd, size, pts, seq}──► encode: mpp_buffer_import(fd) ──► 直接编码
```

此时**一致性问题从"需要小心同步"退化成"根本不存在"**：RGA 和 MPP 都通过 `dma_buf_map_attachment()` 拿 sg_table 后直接 DMA，中间没有 CPU cache 参与，`DMA_BUF_IOCTL_SYNC` 也就不再需要。

剩下的只有**设备间时序同步**（谁先谁后），那是 dma-fence 或 RGA 的 `sync=0` fence 模式的领域，和 cache 无关。

| 对比项 | 当前（像素环） | 零拷贝（fd 环） |
| --- | --- | --- |
| 每帧拷贝次数 | 2 次全量 3.11 MB | 0 |
| 内存流量 @30fps | ~373 MB/s | 0（只有像素本身的 DMA 流量） |
| cache 同步 | 必须严格配对 | 不需要 |
| 额外复杂度 | 无 | buffer 池 + 引用计数 + 生命周期管理 |
| 主要风险 | 偶发花屏（同步遗漏） | buffer 复用冲突、use-after-free |

复杂度只是从"cache 一致性"转移到了"资源生命周期"，并没有消失。这也是为什么建议**先把推流链路跑通再动这块**——它是一个独立且有风险的改造。

---

## 六、验证与调试

### 6.1 五步验证法

从便宜到贵，逐级排除：

1. **确认 buffer 来源**

   ```bash
   cat /sys/kernel/debug/dma_buf/bufinfo
   ```

   看 `rga_buf` / `enc_buf` 的 exporter 是不是 `system`。只有 non-coherent 来源才需要显式 sync。

2. **确认 ioctl 真的生效**

   临时在 `dma_buf_sync_flag()` 里打印每次返回值，跑一分钟。只要出现一次走了 `ENOTTY`/`EINVAL` 分支，就说明在裸奔。

3. **故意制造陈旧读**（决定性验证）

   在 capture 侧 `begin` **之前**，先把 `dst_ptr` 整块读一遍（读进一个 `volatile` 累加变量），人为把旧数据灌进 cache。如果 `begin` 的 invalidate 正常，后续 memcpy 出来的仍是新帧；如果失效，会立刻看到上一帧的内容。这是因果最直接的验证。

4. **对比法**

   把 heap 换成 `system-uncached` 跑一次。花屏消失 ⇒ 确实是一致性缺口；花屏仍在 ⇒ 问题在别处（stride、格式、buffer 复用时序），不必再往一致性上查。

5. **性能基线**

   ```bash
   perf stat -e cache-misses,cache-references ./rkCam -r /root/test.h264
   ```

   确认 sync 的代价在几十 µs 量级，而不是 ms 量级；否则说明 buffer 太大或频率太高。

### 6.2 症状对照表

| 症状 | 更可能的原因 |
| --- | --- |
| 整帧绿/紫/红蓝互换 | 像素格式或 RGA `format` 写错，**不是**一致性问题 |
| 阶梯状/斜向撕裂 | stride（`wstride`/`hstride`）不匹配 |
| 偶发"上一帧"残影 | 缺 invalidate（READ 方向同步缺失） |
| 下半帧或几条横向带错乱 | 同步粒度/对齐问题，或 buffer 在同步窗口内被设备复用 |
| 前几帧正常、之后全花 | CPU 脏行回写覆盖设备数据（缺 `end(WRITE)` 的 clean） |
| 每次必现、位置固定 | 逻辑错误，不是 cache |
| 只在负载高/长时间运行出现 | 典型的 cache 时序问题 |
| 换 `system-uncached` 就好了 | 确认是一致性缺口 |

---

## 七、小结

| 问题 | 答案 |
| --- | --- |
| 怎么解决的 | non-coherent dma-buf + `DMA_BUF_IOCTL_SYNC` 显式同步，**不是**硬件一致性 |
| 在哪些边界同步 | 只有"跨 CPU"的两处：RGA→内存（invalidate）、内存→MPP（clean） |
| 方向写对了吗 | 对了：采集侧 `READ`，编码侧 `WRITE` |
| V4L2 侧要管吗 | 不用，vb2 在 QBUF/DQBUF 自动同步 |
| `begin(WRITE)` 做什么 | arm64 上空操作；真正起作用的是 `end(WRITE)` 的 clean |
| 最大隐患 | `ENOTTY/EINVAL` 静默降级，把一致性问题变成偶发花屏 |
| 带宽代价 | 2 次全量拷贝，约 373 MB/s @30fps（含读侧） |
| 终极方案 | fd 环不走像素，一致性问题自然消失 |

**改进优先级**：① 去掉静默降级 → ② 收窄 sync 窗口 → ③ 零拷贝 fd 环改造。不要用"换 uncached heap"这种用性能换正确性的偷懒做法。

---

## 附：参考资料

- Linux 内核 `Documentation/driver-api/dma-buf.rst` —— `begin_cpu_access` / `end_cpu_access` 的语义定义
- `Documentation/core-api/dma-api.rst` —— streaming DMA mapping 的所有权模型
- `arch/arm64/mm/dma-mapping.c` —— `arch_sync_dma_for_cpu/device` 的具体实现，看方向与动作的对应关系
- ARM Architecture Reference Manual —— `DC IVAC` / `DC CVAC` 对脏行的行为约束
- `drivers/dma-buf/heaps/system_heap.c` —— RK 上 dma-heap 的 sync 回调实现