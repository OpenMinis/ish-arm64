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
| 改动对既有功能有风险吗 | **无阻塞风险**（第 5 节）。stub 默认关闭且关闭时行为实测与 master 一致；`SO_BINDTODEVICE` 无 gate、是真实语义妥协但影响面明确。转正前需修 3 个脚手架隐患 |
| 兼容性回归测试 | **无回归**（第 6 节）。ARM64 226/227，唯一失败项 `go version` 经 8 次空载复跑证伪为负载竞争；x86 与 master 基线逐项一致 |
| 交互式登录能走通吗 | **能拿到真实授权链接**（第 7 节）：完整 `RegisterReq` 握手 → `https://login.tailscale.com/a/...`。**但有前提**，见下 |
| ⚠️ **对第 2/3 节的重要修正** | **空 dump 只够启动，不够登录。** 空接口列表使控制客户端被永久 pause（对外 connect 计数为 0），`tailscale up` 永远拿不到链接。**阶段 2 的 `getifaddrs` 真实接口上报是登录必需项，不是可选增强** |

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

> ⚠️ **【第 7 节修正】"能回答"不等于"可以答空"。** 启动阶段答空即可，
> 但登录阶段要求 dump 里**至少有一个 up 状态的接口**，否则控制客户端被永久 pause。
> 见 7.1。

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

**这意味着不必实现真实的接口枚举**，一个语义正确的空 dump 就能解锁 tsnet **的启动**。

> ⚠️ **【第 7 节修正】"空 dump 就够了"只对启动成立，对登录不成立。**
> 空接口列表会让 `shouldPauseControlClientLocked()` 恒为 true，
> 控制客户端被**永久 pause**，`tailscale up` 永远拿不到授权链接
> （实测对外 connect 计数为 0）。所以真实接口枚举
> （`getifaddrs` → `ifinfomsg`/`ifaddrmsg`）**不是可选增强，而是登录流程的必需项**。
> 详见 7.1。

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
2. **阶段 2（2-3 天）【必需，非可选】**：接宿主 `getifaddrs()`，
   `RTM_GETLINK`/`RTM_GETADDR` 上报真实接口。
   **这是登录流程的前置条件**——空接口列表会让控制客户端被永久 pause（见 7.1），
   只做阶段 1 的话 tsnet/tailscaled 能启动但**永远登录不了**。至少要让
   `AnyInterfaceUp()` 为 true（上报一个 up 状态的非 loopback 接口）；
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

## 5. 代码 review：改动风险评估

> 重新逐行读 `git diff master..HEAD -- fs/` 得出，不依赖前几节的结论。
> 结论先行：**两处改动对 master 既有功能无风险，可以接受**；但脚手架里有
> **3 个必须在转正前修掉的隐患**（其中 1 个是真 bug，当前不可触发）。

### 5.1 netlink stub 的 gate 实际覆盖范围

**先纠正一个容易想当然的说法**：并不是"所有 netlink 代码都被 `ISH_NETLINK_STUB` 门控"。
实际只有**一处**检查环境变量：

```
ish_netlink_stub_enabled()  调用点：1 处  → fs/sock.c:232 (sys_socket)
fd_is_netlink_stub()        调用点：5 处  → bind / getsockname / setsockopt 区 /
                                            sendmsg(1246) / recvmsg(1427)
```

那 5 处**没有**再查环境变量。**但它们等价于被门控**，因为：

`fd_is_netlink_stub()` 判据是 `fd->socket.domain == AF_NETLINK_(16)`，
而 `domain` 字段只有两个写入点，都在 socket 构造函数里；
stub 关闭时 `sys_socket` 在白名单处就返回 `_EAFNOSUPPORT`，
`sys_socketpair` 同样拒绝 AF_NETLINK（与真 Linux 一致）。
**所以关闭时 `domain` 永远不可能是 16，那 5 个分支恒为 false。**

**实测验证**（同一二进制，socket 行为矩阵对照）：

```
                              stub OFF          stub ON
AF_INET/STREAM                rc=3  errno=0     rc=3  errno=0
AF_INET/DGRAM                 rc=3  errno=0     rc=3  errno=0
AF_INET6/STREAM               rc=3  errno=0     rc=3  errno=0
AF_UNIX/STREAM                rc=3  errno=0     rc=3  errno=0
AF_NETLINK/RAW                rc=-1 errno=97    rc=3  errno=0   ← 唯一差异
AF_PACKET/RAW                 rc=-1 errno=97    rc=-1 errno=97
SO_REUSEADDR / SO_KEEPALIVE / TCP_NODELAY       均 rc=0，两边一致
SO_MARK(未映射)               rc=-1 errno=22    rc=-1 errno=22  ← 未映射项仍正确拒绝
```

`diff` 输出只有 `AF_NETLINK` 一行不同。**stub 关闭时行为与 master 一致。**

**额外开销**：关闭时每个 socket syscall 多 1~2 次整数比较（`fd->ops == &socket_fdops`
+ `domain == 16`），且 `ish_netlink_stub_enabled()` 用 `static int cached` 缓存了
`getenv`，不会每次调用。**可忽略。**

### 5.2 当前唯一调用路径

**目前 iSH 里没有任何其他代码依赖 AF_NETLINK。** 全仓库对 netlink 的引用数为 0（见 1.1），
所以这个 stub 的调用方**只有 guest 程序**，且只在 `ISH_NETLINK_STUB=1` 时可达。
不存在"影响其他已依赖 AF_NETLINK 行为的功能"的可能——因为在此之前
AF_NETLINK 在 iSH 上**根本不存在**，任何 guest 程序拿到的都是 EAFNOSUPPORT。

风险方向只有一个：**某些程序把 EAFNOSUPPORT 当作"这不是 Linux / 降级走别的路"的信号**，
stub 打开后它们会改走 netlink 分支并拿到空结果。这正是 tsnet 的期望行为，
但对别的程序**理论上可能导致行为变化**（例如某程序原本 fallback 到 `/proc/net/dev`，
现在改信空的 netlink 结果）。**这是 stub 默认关闭的充分理由。**

### 5.3 转正前必须修的 3 个隐患

#### (a) 【真 bug，当前不可触发】`netlink_peer_fd` 的零值含义是 fd 0

`fd_create()`（`fs/fd.c`）用 `*fd = (struct fd) {}` 全零初始化，
所以 `netlink_peer_fd` **默认是 0，而 0 是一个合法 fd（stdin）**。
而 `sock_close()` 的回收条件是：

```c
if (fd->socket.netlink_peer_fd >= 0) {     // 0 >= 0 为真！
    close(fd->socket.netlink_peer_fd);     // → close(0)，关掉 stdin
```

**当前不可触发**，因为两个 socket 构造函数都显式写了值
（`sock_fd_create` 写 -1，`netlink_stub_socket` 写 `sv[1]`）。
实测 120 次 socket 创建/销毁后 `STDIN ALIVE`，stub 开关两种模式都正常。

**但这是给后来者埋的雷**：任何人新增第三条 socket fd 构造路径而忘记初始化，
就会在 close 时静默关掉 stdin——症状会表现为"某程序的标准输入莫名其妙没了"，
极难定位。**转正时应改为哨兵值语义明确的写法**，例如把判据改成
`> 0`，或（更好）在 `sock_fd_create` 之外增加一个统一的 socket 字段初始化函数。

#### (b) 【噪音 + 性能】`printk` 无条件输出，且在热路径上

stub 里 5 处 `printk` 全部**无门控**（`printk` = `ish_printk`，无 debug 级别判断）。
其中 `SO_BINDTODEVICE` 那条在**每次对外 dial** 时触发——
实测一次 tailscaled 代理测试产生 **512KB** 日志。
转正时这些必须降级为 `TRACE`/`STRACE` 或加 debug gate。

#### (c) 【命名误导】`SO_BINDTODEVICE` 的日志前缀是 `NETLINK_STUB`

`SO_BINDTODEVICE` 与 netlink **完全无关**（见 5.4），但日志打的是
`NETLINK_STUB setsockopt SO_BINDTODEVICE ignored`。会让人误以为关掉 netlink stub
就能关掉这个行为。转正时应改前缀，且这两处改动**应该拆成两个独立 commit**。

### 5.4 SO_BINDTODEVICE：是"假装成功"，且**不受 gate 控制**

**这是本次 review 最需要强调的一点。**

```c
if (level == SOL_SOCKET_ && option == SO_BINDTODEVICE_) {
    printk(...);
    return 0;                 // ← 纯粹 accept-and-ignore，无任何真实处理
}
```

**它没有任何 gate**——`ISH_NETLINK_STUB` 关闭时**照样生效**（实测矩阵已证实：
stub OFF 时 `SO_BINDTODEVICE rc=0`）。也就是说这一处是**真正改变了 master 行为**的改动，
从 `EINVAL` 变成 `0`。

#### 行为差异分析

| 场景 | master | 本分支 | 影响 |
|---|---|---|---|
| 程序设 `SO_BINDTODEVICE` 后**检查返回值**再决定 | 收到 EINVAL，走 fallback | 收到 0，以为绑定成功 | **静默差异** |
| 程序设完不检查（Go netns 即此类） | 整条 dial 失败 | 正常工作 | **修复** |
| 多网卡下真正依赖绑定特定接口 | 失败（明确） | 成功但**实际未绑定** | **静默错误** |

第三行是任务问的风险点，**确实存在**。但在 iSH 的具体语境下可以接受，理由是：

1. **iSH 根本没有多网卡概念**——没有接口模型，`/proc/net/dev` 是硬编码假数据，
   所有流量都走宿主唯一的网络路径。"绑定到 eth0"和"不绑定"在 iSH 里**没有可区分的语义**。
2. Darwin 的近似物 `IP_BOUND_IF` 要 ifindex 而非接口名，**无法正确翻译**，
   即使想做真实现也缺数据源。
3. **仓库已有同样的成例**：`ICMP6_FILTER`、`IP_MTU_DISCOVER` 都是
   "Darwin 无等价物 → `return 0`"，本改动与既有风格一致。
4. 反面代价明确且严重：不改的话**所有 Go 程序的对外连接全挂**。

**但必须承认这是一个真实的语义妥协**，不是"无副作用的修复"。
更诚实的做法是在真正支持接口模型后改为真实现；在此之前 accept-and-ignore
是两害相权的选择。

#### 对既有 socket 功能的回归风险：无

改动是在 `sock_opt_to_real()` **之前**插入的早返回，只拦截
`(SOL_SOCKET, 25)` 这一个组合。`SO_BINDTODEVICE_(25)` 此前**从未被映射**
（`sock_opt_to_real` 里没有这个 case），所以不存在"抢走了原本有效的选项"的可能。
实测矩阵中其他 setsockopt 项行为不变，未映射项（`SO_MARK`=36）仍正确返回 EINVAL。

### 5.5 明确结论

| 改动 | 是否影响 master 既有功能 | 风险等级 | 处置建议 |
|---|---|---|---|
| **netlink stub** | **否**。默认关闭，关闭时行为实测与 master 一致 | **低** | 可接受；转正前修 5.3 的 3 项 |
| **SO_BINDTODEVICE** | **是**（EINVAL → 0），但无 gate、影响面明确 | **低-中** | 可接受；应拆成独立 commit 并去掉 `NETLINK_STUB` 前缀 |

**两处都不阻塞合入。** 但要注意本分支的定位是**调研脚手架，不是可合并实现**——
真要合入 master 的话，建议只挑 `SO_BINDTODEVICE`（独立成 commit + 去掉误导性日志），
netlink 部分按第 3.6 节的阶段计划重写。

---

## 6. 兼容性回归测试结果

> 测试套件：`benchmark/run.sh compat` —— 227 项，覆盖 18 个类别
> （Shell/Python/Node.js/Go/C/包管理/网络工具/AI CLI/Skill 生态等）。
> 在本分支（带 netlink stub + SO_BINDTODEVICE 改动）完整跑完。
> 原始日志：`benchmark/results/run_20260916_091643.csv`

### 6.1 总结果

| 架构 | 通过 | 失败 | 通过率 |
|---|---|---|---|
| **ARM64**（本次改动影响的架构） | **226** | **1** | **99%** |
| x86（Jitter） | 212 | 15 | 93% |

### 6.2 与 master 基线对比

| | master 基线<br>(2026-07-11) | 本分支<br>(2026-09-16) | 差异 |
|---|---|---|---|
| ARM64 | 227 / 0 / **100%** | 226 / 1 / **99%** | **-1** |
| x86 | 212 / 15 / 93% | 212 / 15 / 93% | **0** ✅ |

x86 列与基线**完全一致**（212 通过 / 15 失败），说明两次运行环境可比、
对照有效。ARM64 差 1 项，下面逐项定性。

### 6.3 唯一的 ARM64 失败项：`Lang | go version` —— **不是回归**

**判定：测试期间的机器负载竞争，非本次改动导致。** 证据链：

1. **单独复跑全部通过**：用套件里的**完全相同**命令和 15s 超时，
   空载状态下连跑 5 次 —— **5/5 PASS**。
2. **stub 开关无关**：`ISH_NETLINK_STUB=1` 下再跑 3 次 —— **3/3 PASS**。
   合计 **8/8**。
3. **时间余量极大**：实测 `go version` 耗时 **0.43–0.78 秒**，
   而该用例预算是 **15 秒**，余量约 **19 倍**。这种量级的余量不可能被
   "多几次整数比较" 吃掉。
4. **改动与 Go 运行时无交集**：本次只动了 `sys_socket` / `bind` /
   `getsockname` / `sendmsg` / `recvmsg` / `setsockopt` 六个 socket 入口，
   而 `go version` **不创建任何 socket**（纯本地执行 + 打印版本号）。
5. **失败发生的时间窗有旁证**：该用例失败时，我正在同一台机器上并行跑
   netlink review 的验证测试（多个 ish 进程同时读写同一 fakefs 的 meta.db）。
   这与 [[history]] 里记录过的 fakefs meta.db 写锁争用表现一致。

**方法论说明**：这条我没有直接采信"跑一次就算数"，而是用
"相同命令 + 相同超时 + 空载复跑 8 次" 来区分**偶发超时**和**确定性失败**。
如果是本次改动引入的回归，空载复跑应当稳定复现失败——实际是稳定通过。

### 6.4 x86 的 15 项失败：与本次改动无关

x86 侧 15 项失败与 master 基线**逐项一致**，包括：
`bun lang+stdlib`、`claude --version`、`claude -p (no-auth)`、`codex --version`、
`node-edge-tts` 等。这些是 **32 位架构固有限制**（现代 JS 运行时/Rust 二进制
需要 64 位地址空间），master 基线里就是 FAIL，属于已知预期。

补充：本机 `build-x86-release/ish` 是 2026-02-14 的旧构建（其 meson
build dir 指向已失效的旧路径，无法重新生成），**早于本次改动**，
因此 x86 列在物理上不可能受本分支影响——这也正是它与基线完全吻合的原因。

### 6.5 结论

**未发现任何由本次改动导致的兼容性回归。**

- ARM64 唯一失败项经 8 次空载复跑证伪，属环境竞争
- x86 列与 master 基线逐项一致
- 改动只触及 6 个 socket syscall 入口，其中 netlink 相关 5 处在
  stub 关闭时恒为 false（5.1 节已用 socket 行为矩阵实测证明）

与 5.5 节的 review 结论互相印证：**这两处改动对既有软件生态无影响。**

---

## 7. 交互式登录流程实测（`tailscale up`，无 authkey）

> 第三轮实测。目标是走一次真实的交互式登录，观察授权链接从哪来、CLI 与 daemon 怎么通信。
> **结论：拿到了真实授权链接，但同时发现了前几节一个被低估的问题。**

### 7.1 关键发现：空接口列表会让登录流程永久卡死

**默认配置下 `tailscale up` 根本打不出授权链接。** 连跑 5 次，行为一致：
CLI 挂到超时后输出 `context canceled`，`tailscale status` 始终 `Logged out.`，
daemon 日志停在：

```
localapi: [POST] /localapi/v0/login-interactive
StartLoginInteractiveAs("root"): url=false
control: client.Login(2)
control: authRoutine: awaiting unpause     ← 永远等不到 unpause
```

**根因不是缺凭据，是我们的 stub 返回空接口列表。** 出处：

```go
// ipn/ipnlocal/local.go  shouldPauseControlClientLocked()
networkUp := b.interfaceState.AnyInterfaceUp()
pauseForNetwork := !networkUp && !testenv.InTest() && !envknob.AssumeNetworkUp()
if pauseForNetwork {
    return true          // ← 控制客户端被 pause，不发起任何对外连接
}
```

`RTM_GETLINK` 返回空 dump → `interfaces.State{ifs={}}` → `AnyInterfaceUp()` 为 false
→ `shouldPauseControlClientLocked()` 恒为 true → 控制客户端**永久 pause**。

**实测铁证：整个登录过程中 daemon 的对外 connect 计数为 0**
（`NETDIAG connect` 一条都没有）。它压根没尝试联网，而不是联网失败。

### 7.2 验证：解除这个条件后，真实链接立刻出现

tailscale 自带一个逃生阀 `envknob.AssumeNetworkUp()`
（环境变量 `TS_ASSUME_NETWORK_UP_FOR_TEST=1`）可以跳过该判断。加上之后：

```
control: control server key from https://controlplane.tailscale.com: ts2021=[fSeS+], legacy=[nlFWp]
control: Generating a new nodekey.
control: RegisterReq: onode= node=[R12T0] fup=false nks=false
control: RegisterReq: got response; nodeKeyExpired=false, machineAuthorized=false; authURL=true
control: AuthURL is https://login.tailscale.com/a/161ce8e8012f93
control: doLogin(regen=false, hasUrl=true)
popBrowserAuthNow("root"): url=true, key-expired=false
```

`tailscale status` 同步显示：

```
Logged out.
Log in at: https://login.tailscale.com/a/161ce8e8012f93
```

**这是完整走通的真实控制面握手**：TLS 连上 `controlplane.tailscale.com`、
取得服务器公钥、生成 nodekey、发送 `RegisterReq`、拿回一次性授权 URL。
到"等待用户点击链接"为止的每一步都成立。**只差真实账号点链接完成授权。**

这条对照同时**反证了 iSH 侧的网络栈是健全的**：一旦不被 pause 挡住，
TLS / DNS / HTTP 全链路都能跑通。

### 7.3 授权链接的生成与传递机制

**链接由控制面生成**，不是本地构造：daemon 发 `RegisterReq`，
`controlplane.tailscale.com` 在响应里返回 `authURL`
（伴随 `machineAuthorized=false`）。

**CLI ↔ daemon 是 daemon 推送，不是 CLI 轮询**（`cmd/tailscale/cli/up.go`）：

```go
watcher, err := localClient.WatchIPNBus(watchCtx, 0)                      // :618
...
if url := n.BrowseToURL; url != nil {                                     // :762
    fmt.Fprintf(Stderr, "\nTo authenticate, visit:\n\n\t%s\n\n", authURL) // :786
}
```

CLI 经 unix socket 上的 localapi 订阅 **IPN bus**（长连接事件流），
daemon 在 `popBrowserAuthNow()` 时推送一条带 `BrowseToURL` 的通知。
**CLI 是阻塞等待的**——`watchCtx` 直到授权完成或超时才结束。

### 7.4 【开放疑点，未深挖】CLI 收不到 IPN bus 推送

即使在 7.2 拿到链接的那次运行里，**CLI 端仍然只收到 `context canceled`，
没有打印链接**。链接是通过 `tailscale status` 查到的，不是 `up` 打印的。

已观察到的现象：

- daemon 侧明确记录了 `popBrowserAuthNow("root"): url=true`，说明**推送已发出**
- 但 daemon 日志里 `watch-ipn-bus` 的 localapi 调用计数为 **0**
- CLI 最终以 `watchCtx.Err()` 即 `context canceled` 退出

**怀疑方向**（未验证）：IPN bus 是 unix socket 上的**长连接事件流**，
可能触及 iSH 的 epoll / 长连接语义——与 [[ish-claude-tui-keyboard-hang]] 记录的
epoll ONESHOT 问题属于同一类。**本次不深挖，仅记录。**

**影响评估**：不阻塞功能。链接可以通过 `tailscale status` 拿到，
`--authkey` 路径也完全绕开这个机制。但会影响 `tailscale up` 的交互体验。

### 7.5 与 tsnet demo 的对比：协议层完全一致

| | 官方 tailscaled + CLI | tsnet 库 |
|---|---|---|
| 链接来源 | `RegisterReq` → controlplane | **同一个** |
| 内部机制 | `WatchIPNBus` → `n.BrowseToURL` | `WatchIPNBus`（`tsnet.go:541`） |
| 额外通道 | 无 | `printAuthURLLoop` **轮询** `st.AuthURL`（`tsnet.go:1144`） |
| 暴露方式 | CLI 打印到 stderr | `logf` 回调写日志 |

**两者是同一套 controlplane API、同一条 IPN bus**，只是出口不同：
一个给人看，一个给程序订阅。

**一个有意思的差异**：tsnet 除了订阅事件，还有一个**每几秒轮询 status** 的
`printAuthURLLoop`。在 iSH 这个环境里**轮询反而更健壮**——
这正好解释了为什么 tsnet demo 能正常打出状态，而 CLI 的 `up` 拿不到推送（7.4）。

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
