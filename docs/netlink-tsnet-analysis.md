# 让 tsnet 在 iSH ARM64 上跑起来：netlink 支持调研

> 分支 `explore/netlink-tsnet`。调研 + 原型验证，**不是可合并的实现**。
> 所有数字来自本机实测（macOS CLI，`build-arm64-release/ish` + `alpine-arm64-321` realfs）。

## TL;DR

| 问题 | 结论 |
|---|---|
| iSH 支持 AF_NETLINK 吗 | **完全不支持**，零实现（不是部分覆盖） |
| tsnet 到底要哪些 netlink 消息 | **只要 `RTM_GETLINK`**，实测 2 次；**没有任何 `RTM_GETROUTE`** |
| 能不能在应用层绕过 | **不能。** 调用点在 Go **stdlib** `net` 包，无 build tag / 无环境变量 / 无 fallback |
| 内核侧改动量 | **小。** 一个"空 dump"桩 ≈ 250 行，已原型验证 |
| 做完之后 tsnet 能起来吗 | **能。** `s.Start()` 返回 OK，WireGuard up，进入 `NeedsLogin` |
| 值得做吗 | **值得**，但要做成真实现而非桩；另有一个与 netlink 无关的坑必须一起修 |
| **官方 tailscaled `--tun=userspace-networking` 呢** | **也能跑起来**（第 4 节）：启动到 `NeedsLogin`，SOCKS5/HTTP 代理端口均 `LISTENING`，无需额外 syscall |

---

## 1. 现状调研：完全没有实现

### 1.1 代码位置

分发点是 `fs/sock.c:38` 的 `sys_socket()`，协议族白名单在 `fs/sock.h`：

```c
static inline int sock_family_to_real(int fake) {
    switch (fake) {
        case PF_LOCAL_: return PF_LOCAL;
        case PF_INET_:  return PF_INET;
        case PF_INET6_: return PF_INET6;
    }
    return -1;                       // ← AF_NETLINK(16) 落这里
}
```

```c
int_t sys_socket(dword_t domain, dword_t type, dword_t protocol) {
    int real_domain = sock_family_to_real(domain);
    if (real_domain < 0)
        return _EAFNOSUPPORT;        // ← errno 97，即报错来源
    ...
}
```

**白名单只有 3 个协议族。** 全仓库 `kernel/` + `fs/` + `emu/` + `util/` + `asbestos/` + `app/`
对 `netlink|rtnetlink|NETLINK_ROUTE|rtmsg|nlmsghdr|ifinfomsg|ifaddrmsg` 的命中数为 **0**
（vendored 的 `musl-1.2.4/` 和 `busybox-1.36.0/` 里有，但那是被模拟的 guest 代码，不是 iSH 实现）。

所以是**完全没实现**，不存在"实现了一部分消息类型"的情况。

### 1.2 一个决定性的架构事实

> **iSH 的 socket 层是薄转发层，没有自己的网络协议栈。**

`sys_socket` 直接调宿主 `socket()`；`socket_fdops` 的 read/write/poll/ioctl 全部转发给宿主 fd
（`.ioctl = realfs_ioctl`）。

**因此 netlink 不可能像 AF_INET 那样"转发给宿主"——Darwin 根本没有 AF_NETLINK**
（它用 `PF_ROUTE` / `sysctl(NET_RT_DUMP)`，报文格式完全不同）。任何 netlink 支持
都必须在 iSH 内部**从零合成报文**。这是工作量的主要来源，也是它区别于其他 syscall 补全的地方。

### 1.3 没有可复用的数据源

- `fs/proc/net.c` 整个 `/proc/net` **只有一个 `dev` 节点，且是硬编码假数据**
  （全零统计 + 写死的 `lo`/`eth0`，注释自称 "dummy"）
- **没有 `/proc/net/route`**
- 全仓库零处调用 `getifaddrs` / `SIOCGIFCONF` / `if_nametoindex`

也就是说 iSH **没有任何网络接口模型**。这对后面的方案选择很关键。

---

## 2. 缺口分析：需要的比预期少得多

### 2.1 纠正一个前提

任务描述推测是 `RTM_GETROUTE`（路由表查询）。**实测不是。**

真正的调用点在 **Go 标准库**，不在 tailscale：

```go
// $GOROOT/src/net/interface_linux.go:17
func interfaceTable(ifindex int) ([]Interface, error) {
    tab, err := syscall.NetlinkRIB(syscall.RTM_GETLINK, syscall.AF_UNSPEC)
    if err != nil {
        return nil, os.NewSyscallError("netlinkrib", err)   // ← 报错字符串来源
    }
    ...
}

// $GOROOT/src/net/interface_linux.go:124
func interfaceAddrTable(ifi *Interface) ([]Addr, error) {
    tab, err := syscall.NetlinkRIB(syscall.RTM_GETADDR, syscall.AF_UNSPEC)
    ...
}
```

Go 的 `net` 包在 `GOOS=linux` 下只用这两种消息：**`RTM_GETLINK`(18)** 和 **`RTM_GETADDR`(22)**。
**完全不涉及 `RTM_GETROUTE`(26)。** `netmon` 里的 `route.FetchRIB` / `NET_RT_DUMP` 只在
`interfaces_bsd.go` / `interfaces_darwin.go` 里，Linux 构建目标下根本不编译。

### 2.2 实测佐证

给原型桩加上 RTM 类型日志，跑真实 tsnet 程序，全程只看到：

```
total netlink replies: 2
GETLINK=2  GETADDR=0  GETROUTE=0
```

（`RTM_GETADDR` 的 socket 被创建过，但只做了 `getsockname` 就放弃，没发请求——
因为 `RTM_GETLINK` 返回空接口列表后，上层没有接口可查地址。）

**结论：最小可用集就是能回答一次 `RTM_GETLINK` dump。**

### 2.3 需要的数据结构

做成"诚实的空 dump"只需要一个结构：

```c
struct nlmsghdr_ {          // 16 字节，全 Linux 架构一致
    uint32_t nlmsg_len;
    uint16_t nlmsg_type;    // NLMSG_DONE = 3
    uint16_t nlmsg_flags;
    uint32_t nlmsg_seq;     // 必须回显请求的 seq
    uint32_t nlmsg_pid;
};
struct sockaddr_nl_ {       // 12 字节
    uint16_t nl_family; uint16_t nl_pad;
    uint32_t nl_pid; uint32_t nl_groups;
};
```

要做成**真实现**（上报真实接口）还需要 `ifinfomsg` + `ifaddrmsg` + `rtattr` TLV 编码，
数据源用宿主 `getifaddrs()`。但 tsnet **不需要**这一层就能起来（见下）。

### 2.4 工作量级别

**小补丁。** 原型实测 ≈ 250 行，集中在 `fs/sock.c` 一个文件。

| 触点 | 改动 | 说明 |
|---|---|---|
| `sys_socket` | +4 行 | 在白名单前拦截 AF_NETLINK |
| `sys_bind` | +5 行 | 接受并忽略（nl_groups 订阅无意义，因为不推事件） |
| `sys_sendmsg` / `sys_recvmsg` | 各 +40 行 | **必需**，见下 |
| `sys_getsockname` | +15 行 | **必需**，见下 |
| `sock_close` | +4 行 | 回收 socketpair 另一端 |
| `fs/fd.h` | +1 字段 | `int netlink_peer_fd` |

**实现技巧**：用宿主 `socketpair(AF_UNIX, SOCK_DGRAM)` 做载体，`fd->real_fd` 给 guest，
另一端由内核写回复。这样 poll/epoll/read/close/引用计数**全部复用现成机制**，
只需特化 sendmsg/recvmsg/getsockname 三个入口。

### 2.5 三个不明显的坑（原型踩过，都必须处理）

1. **Go 用 `sendmsg`/`recvmsg`，不是 `send`/`recv`。** 只拦截 sendto/recvfrom 不够。
2. **`msg_name` 是 `sockaddr_nl`，通用路径会在 `sockaddr_read()` 里拒掉 → EINVAL。**
   必须在 fd 查找后、任何 sockaddr 解析前分流。
3. **`getsockname` 必须返回 `sockaddr_nl`。** Go 的 `netlinkrib` 用它拿内核分配的 pid，
   再和回复的 `nlmsg_pid` 比对。返回后备 AF_UNIX 地址会导致 EINVAL——这个最隐蔽。

### 2.6 风险评估：低

- 默认路径零影响：`AF_NETLINK` 之前就返回 EAFNOSUPPORT，现在仍可由 gate 控制
- 不碰 JIT / 内存 / 信号 / 锁序等危险区
- 原型用 `ISH_NETLINK_STUB=1` 运行时 gate，实测 stub-off 行为与 master 完全一致

---

## 3. 可行性结论与建议

### 3.1 应用层绕过：**不可行**（这是本次调研最重要的否定结论）

任务问"tsnet / x/net/route 是否有环境变量或 build tag 可以跳过 netlink"。答案是**没有**，
而且原因比"tailscale 没提供开关"更根本：

**调用点在 Go 标准库 `net` 包里，不在 tailscale 的代码里。**

- `net.Interfaces()` / `net.InterfaceAddrs()` 在 `GOOS=linux` 下**无条件**走 netlink
- `interface_linux.go` 里**没有任何 build tag、GODEBUG 开关或 fallback 分支**
- Go 不走 libc（纯汇编 syscall stub），所以 **`LD_PRELOAD` 也没有注入点**

推论：

- ❌ 环境变量 / build tag — 不存在
- ❌ LD_PRELOAD shim — Go 不经 libc
- ❌ 只改 tailscale — 调用点不在它那里
- ⚠️ 改 Go stdlib 重新编译 toolchain — 技术上可行，但要维护一个 patched Go，
  每个 Go 版本都要跟，**成本远高于在 iSH 里加桩**

**所以"在应用层想办法"这条路对 tsnet 是封死的。必须在 iSH 内核侧解决。**

（反过来说这也是好消息：一旦 iSH 支持了，**所有** Go 程序都受益，不只 tsnet——
任何调用 `net.Interfaces()` 的 Go 程序目前在 iSH 上都是挂的。）

### 3.2 内核侧实现：已原型验证可行

#### 实测 A/B（同一二进制，只差环境变量）

| | stub OFF | stub ON |
|---|---|---|
| C 探针 `socket(AF_NETLINK,...)` | `errno=97` | 成功，收到 `NLMSG_DONE`，seq 正确回显 |
| **tsnet `s.Start()`** | **`netlinkrib: address family not supported`** | **`STEP3: Start() returned OK`** ✅ |

stub ON 时 tsnet 完整走完：

```
using fake (no-op) tun device
link state: interfaces.State{defaultRoute= ifs={} v4=false v6=false}
magicsock: disco key = d:13199dfc10c0740a
Creating WireGuard device...
Bringing WireGuard device up...
wg: Interface state was Down, requested Up, now Up
StartLoginInteractiveAs(""): url=false
control: client.Login(10)
STEP3: Start() returned OK          ← 任务报的错已彻底解决
control: authRoutine / mapRoutine / updateRoutine: awaiting unpause
health(warnable=warming-up): ok
```

之后停在 `NeedsLogin` 等待登录——**这是无凭据时的正确行为，不是故障**。

#### 关键发现：空 dump 就够了

`interfaces.State{ifs={} v4=false v6=false}` 说明 tsnet **接受"这台机器没有网络接口"**
并继续启动。因为 `tsnet` 本来就是纯用户态网络栈（gVisor netstack），
它查接口只是为了做 endpoint 发现优化，不是硬依赖。

**这意味着不必实现真实的接口枚举**，一个语义正确的空 dump 就能解锁 tsnet。
真实现（`getifaddrs` → `ifinfomsg`/`ifaddrmsg`）可以作为后续增强，不是前置条件。

### 3.3 必须一起修的另一个坑（与 netlink 无关）

原型过程中发现的独立缺口，**不修的话 netlink 修好了也连不上网**：

**`SO_BINDTODEVICE`(25) 在 `sock_opt_to_real()` 里完全没有映射**，返回 -1 → EINVAL。
Go 的 netns 代码在**每次对外 dial 时都会设置它**，所以所有出站连接全部失败：

```
dial tcp 199.165.136.101:443: setting SO_BINDTODEVICE: invalid argument
```

修法只有 3 行，且仓库已有成例（`IP_MTU_DISCOVER` / `TCP_CONGESTION` 的
"Darwin 无等价物 → return 0"）：

```c
// iSH 没有接口模型，Darwin 的近似物 IP_BOUND_IF 要 ifindex 而非名字。
// 单一宿主网络路径下无从选择，unbound 即调用方想要的行为。
if (level == SOL_SOCKET_ && option == SO_BINDTODEVICE_)
    return 0;
```

修完后出站 HTTPS 实测可达：`controlplane.tailscale.com` 和 `login.tailscale.com` 都通。

### 3.4 其他已知边界（非阻塞，但要知道）

- **realfs 下 AF_UNIX 文件路径 bind 恒 EPERM**（`realfs_mknod` 对 `S_IFSOCK` 返回 `_EPERM`）。
  10 行 C 即可复现，与 netlink 无关。fakefs 下正常（`fakefs_mknod` 把真实 mode 存进 meta.db）。
  对 tsnet **无影响**（tsnet 是库，不像 tailscaled 那样监听控制 socket）。
- UDP 缓冲区 `setsockopt` 到 7MB 失败 → tsnet 自己降级为警告，只影响吞吐。

### 3.5 综合评估：值得做

**投入产出比是好的。**

投入：
- 空 dump 版 ≈ 250 行 + `SO_BINDTODEVICE` 3 行，集中在 `fs/sock.c`
- 风险低：不碰危险子系统，可 gate，默认路径零影响
- 已有工作原型，改成产品级主要是补 `RTM_GETADDR`/`RTM_GETROUTE` 的正确应答与测试

产出：
- 解锁 **tsnet 全家**（tailscale 嵌入式库，正是本任务目标）
- 解锁**所有调用 `net.Interfaces()` 的 Go 程序**——目前在 iSH 上全是挂的
- 顺带让 busybox `ip` 之类传统工具有路可走

**但建议做成"诚实的最小实现"，不要长期停留在空桩：**

| 消息 | 建议 |
|---|---|
| `RTM_GETLINK` | 用宿主 `getifaddrs()` 至少上报 `lo`（+ 一个默认接口），比空列表更接近真实 |
| `RTM_GETADDR` | 同上，上报对应地址 |
| `RTM_GETROUTE` | 先返回空 dump。真实路由表要解析 Darwin `PF_ROUTE`/`NET_RT_DUMP`，iOS 沙箱可用性**未验证**，成本翻倍且收益不明 |
| 组播事件推送 | **不必做**。tailscale 自带 polling fallback（错误串 `AF_NETLINK RTMGRP failed, falling back to polling` 即为证据） |

**不建议做的**：完整 netlink 协议族（写路由、netfilter、netns）。iSH 没有网络栈，
这些语义无处落地。

### 3.6 分阶段建议

1. **阶段 1（1-2 天）**：空 dump + `SO_BINDTODEVICE`，按仓库惯例加 build option + env 双 gate。
   目标：tsnet `s.Start()` 通过，加回归测试。
2. **阶段 2（2-3 天）**：接宿主 `getifaddrs()`，`RTM_GETLINK`/`RTM_GETADDR` 上报真实接口；
   同一数据源顺带补 `/proc/net/route` 和 `/proc/net/dev`（替掉现在的硬编码假数据）。
3. **阶段 3（可选）**：`PF_ROUTE` 真实路由表。**先在 iOS 上验证 `NET_RT_DUMP` 可用性再投入。**

---

## 4. 官方 tailscaled `--tun=userspace-networking` 实测

> 第二轮实测（同分支）。用**真正的官方二进制**（`tailscale.com/cmd/tailscaled`
> + `cmd/tailscale`，v1.102.4，`GOOS=linux GOARCH=arm64 CGO_ENABLED=0` 静态编译），
> 不是自写 demo。

### 4.1 结论：**能跑起来**

| 检查项 | 结果 |
|---|---|
| 进程启动（不 panic / 不因缺 syscall 退出） | ✅ 正常启动到 `NeedsLogin` |
| netlink 是否仍是阻塞点 | ✅ 已解决（stub OFF 时仍复现原报错） |
| SOCKS5 `127.0.0.1:1055` 监听 | ✅ `LISTENING` |
| HTTP 代理 `127.0.0.1:1056` 监听 | ✅ `LISTENING` |
| `tailscale` CLI ↔ daemon 控制通道 | ✅ `tailscale status` → `Logged out.` |
| 是否需要 netlink 之外的额外 syscall | ✅ 不需要（详见 4.4） |

**架构判断得到证实**：`--tun=userspace-networking` 与 tsnet 库共享同一套
gVisor netstack 路径，所以第 3 节对 tsnet 的结论直接适用于官方 daemon。

### 4.2 A/B 对照（同一二进制，只差 `ISH_NETLINK_STUB`）

```
# stub OFF —— 复现原始报错
$ ./build-arm64-release/ish -r alpine-arm64-321 /tmp/tailscaled \
      --tun=userspace-networking --socks5-server=127.0.0.1:1055 ...
netmon.New: route ip+net: netlinkrib: address family not supported by protocol

# stub ON —— 完整启动
$ ISH_NETLINK_STUB=1 ./build-arm64-release/ish -r alpine-arm64-321 /tmp/tailscaled ...
wgengine.NewUserspaceEngine(tun "userspace-networking") ...
link state: interfaces.State{defaultRoute= ifs={} v4=false v6=false}
magicsock: disco key = d:a91dd7270ac8c0e0
Creating WireGuard device...
Bringing WireGuard device up...
Bringing router up...
Starting network monitor...
Engine created.
got LocalBackend in 27ms
control: authRoutine / mapRoutine / updateRoutine: awaiting unpause
health(warnable=wantrunning-false): error: Tailscale is stopped.   ← 未登录时的正确状态
```

代理监听实测（guest 内 `nc -z`）：

```
--- tailscale status ---
Logged out.
--- SOCKS5 port 1055 ---   SOCKS5 1055 LISTENING
--- HTTP proxy port 1056 --- HTTPPROXY 1056 LISTENING
```

daemon 侧同时记录到 `socks5: client connection failed: could not read packet header`
——正是 `nc -z` 连上又立刻断开的预期表现，**反证 SOCKS5 服务确实在 accept**。

### 4.3 未验证的部分（重要边界）

**没有做带 auth-key 的完整上线验证**，因为手上没有可用的 tailnet 凭据。
所以以下**未经证实**：

- 登录后能否真正建立 WireGuard 隧道 / 打通 DERP
- SOCKS5 代理能否真正转发流量到 tailnet 内的节点

已验证的是**到"等待授权"为止的全部启动路径 + 代理端口可 accept**。
`tailscale up --auth-key=...` 之后的行为需要有凭据时另行验证。

（旁证：上一轮调研中实测过 guest 内到 `controlplane.tailscale.com` /
`login.tailscale.com` 的 HTTPS 可达，说明控制面网络路径本身通畅。）

### 4.4 是否需要额外 syscall 支持：不需要，但有两个坑

**结论：netlink stub + `SO_BINDTODEVICE` 两项之外，不需要新增 syscall 支持。**
daemon 模式用到的 unix socket 控制通道、fd 操作、后台进程管理都已可用。

两个需要注意的既有问题（**都不是 netlink 引入的**）：

**(a) 控制 socket 必须用 abstract 地址（realfs 下）**

realfs 下 `--socket=/path/to.sock` 会失败：

```
safesocket.Listen: listen unix /tmp/tsstate/tailscaled.sock: bind: operation not permitted
```

根因是 `realfs_mknod`（`fs/real.c`）对 `S_IFSOCK` 直接 `return _EPERM`，
所以任何 AF_UNIX **文件路径** bind 在 realfs 下都是 EPERM（10 行 C 即可复现，与 netlink 无关）。
规避：用 abstract socket `--socket=@tsd`（不碰文件系统），实测正常。
fakefs 下无此问题（`fakefs_mknod` 把真实 mode 存进 meta.db，实测 `UNIXBIND OK`）。

**(b) 后台 daemon + shell 退出会触发 illegal-instruction 风暴（既有 bug，非阻塞）**

测试脚本里把 tailscaled 放后台再退出 shell 时，观测到 **61572 条**
`illegal instruction at 0x9c128: insn=0x00000000`。

定位过程：

- `0x9c128` 在 **busybox** 里，反汇编实为 `mov x3, x20`（`aa1403e3`），
  但 iSH 报 `insn=0x0` —— 说明取到的是**空指令流**，不是不支持的编码
- **全部发生在 `SCRIPT_DONE` 之后**（实测：SCRIPT_DONE 在第 20 行，首条 fault 在第 21 行），
  即进程退出竞态期间，**所有功能断言都已通过**
- **与 netlink 无关**：stub ON/OFF 对照，单独跑 busybox `nc`（含 listener 消失场景）
  各 0 条；单独跑长生命周期后台 Go 进程也是 0 条
- **前台跑 tailscaled 100 秒：0 条 fault**（见下）

```
# 前台运行（真实用法），零异常
$ ISH_NETLINK_STUB=1 timeout 100 ./build-arm64-release/ish -r alpine-arm64-321 \
      /tmp/tailscaled --tun=userspace-networking ...
illegal-insn count: 0
```

**所以这是"后台进程 + shell 退出"时的既有 teardown 竞态**（大概率与
[[ish-claude-crash-root-cause]] 记录的退出期竞态同源），
**不影响 tailscaled 的正常前台使用**，也不是本次改动引入的。
但如果将来要把 tailscaled 做成真正的后台 daemon 常驻，这个需要单独查。

### 4.5 对第 3 节结论的修正

第 3 节写"tsnet 是库，不像 tailscaled 那样监听控制 socket，所以 realfs EPERM 对它无影响"
——这点成立，但要补充：**官方 tailscaled 确实受影响**，规避方式是 abstract socket，
成本为零（一个命令行参数）。不构成阻塞。

---

## 附：复现方法

```bash
# 1. 造一个 tsnet 测试程序（GOOS=linux GOARCH=arm64 CGO_ENABLED=0）
#    调用 tsnet.Server{}.Start()

# 2. 基线：复现报错
./build-arm64-release/ish -r alpine-arm64-321 /tmp/tsnetdemo
# → STEP2-FAIL: tsnet: route ip+net: netlinkrib: address family not supported by protocol

# 3. 打上原型桩后
ISH_NETLINK_STUB=1 ./build-arm64-release/ish -r alpine-arm64-321 /tmp/tsnetdemo
# → STEP3: Start() returned OK
```

原型代码在本分支 `fs/sock.c` / `fs/sock.h` / `fs/fd.h`，全部标了 `[STAGE-0 PROBE]`，
**是实验脚手架，不是可合并的实现**。
