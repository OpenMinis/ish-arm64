# AOJIT 用例速度对比：Windows x86_64 (xlat) vs Mac arm64 (AOJIT on/off)

同一套 `benchmark/aojit` 用例（`guest/cases.json` 中 quick 14 项 + full 30 项，共 44 项）与同一批 guest 脚本，
分别在两边运行，直接比较墙钟耗时（每项取 2 次中较好的一次，与 Mac 基线的取法一致）。

| | Windows（本次） | Mac（基线） |
|---|---|---|
| 主机 | Windows 11，i5-13600K，4GB 内存 | Mac（Apple Silicon） |
| ish | `port/windows` 957094fe，`build-x86_64-jit`（x86_64 guest，指令级翻译器 xlat） | build 822，arm64 guest，JIT |
| 模式 | `--modes cur`（x86_64 没有 AOT 镜像） | 同一二进制，`/proc/ish/jit` 切 AOJIT on / off |
| rootfs | Alpine 3.21 x86_64，按 cases.json 的 needs 补装包 | alpine-arm64-321-full |
| 数据 | 2026-10-11 本次运行 | `alpine-arm64-321-full/tmp/aojit/results/mac_{quick,full}.txt`（2026-09-26） |

## 汇总

可比用例 43 项（`shell.selfmod` 不适用，见下）：

- **Windows 相对 Mac AOJIT on：几何平均 1.91× 慢，总耗时 2.27×**（408.1s vs 179.5s）。
- Windows 相对 Mac AOJIT off：几何平均 0.82×（总耗时 0.92×），即整体已与 Mac 不开镜像时相当或略快。

差距最大的用例（Windows / Mac-on）：

- net.requests_cold requests 冷启动 x5: 5.15x (win 18.35s, mac on 3.56s, off 8.69s, win/off 2.11x)
- py.t02_json_stdin agent: stdin JSON x20: 3.52x (win 8.05s, mac on 2.29s, off 4.58s, win/off 1.76x)
- node.json node JSON: 3.51x (win 1.72s, mac on 0.49s, off 1.04s, win/off 1.65x)
- net.node_https node https(未录制): 3.50x (win 6.89s, mac on 1.97s, off 3.54s, win/off 1.95x)
- py.t03_json_key agent: 读配置 x20: 3.44x (win 7.87s, mac on 2.29s, off 4.57s, win/off 1.72x)
- py.t01_startup agent: python 启动 x20: 3.43x (win 4.43s, mac on 1.29s, off 2.39s, win/off 1.85x)
- node.exec node 调子进程: 3.40x (win 2.65s, mac on 0.78s, off 1.10s, win/off 2.41x)
- bb.all busybox 全家桶(录制负载): 3.11x (win 188.56s, mac on 60.54s, off 136.04s, win/off 1.39x)

差距最小（Windows 反而更快或接近）：

- shell.bzip2 bzip2 8MB: 0.36x (win/off 0.35x)
- shell.pipeline shell 文本管道: 1.05x (win/off 0.36x)
- family.zlib 旧版 zlib: 1.16x (win/off 0.56x)
- py.t11_sqlite agent: sqlite 聚合: 1.16x (win/off 0.52x)
- bb.pipeline busybox 管道: 1.22x (win/off 0.40x)
- py.t05_log_scan agent: 27MB 日志正则: 1.25x (win/off 0.26x)

## 逐项对照

倍数 = Windows 耗时 / Mac 耗时，>1 表示 Windows 更慢。

| # | 用例 | 档 | 说明 | Windows xlat | Mac AOJIT off | Mac AOJIT on | Win / Mac-on | Win / Mac-off |
|---|---|---|---|---:|---:|---:|---:|---:|
| 1 | `py.pip_list` | quick | pip list | 2.90s | 3.51s | 1.37s | **2.12×** | 0.83× |
| 2 | `py.startup_json_x10` | quick | python 启动+json x10 | 2.96s | 2.27s | 1.12s | **2.64×** | 1.30× |
| 3 | `zlib.roundtrip` | quick | zlib 压缩/解压 | 0.42s | 0.77s | 0.28s | **1.50×** | 0.55× |
| 4 | `bb.pipeline` | quick | busybox 管道 | 0.50s | 1.25s | 0.41s | **1.22×** | 0.40× |
| 5 | `bb.ash_loop` | quick | ash 循环 | 1.08s | 2.75s | 0.53s | **2.04×** | 0.39× |
| 6 | `bb.grep_r` | quick | grep -r | 1.81s | 2.10s | 0.78s | **2.32×** | 0.86× |
| 7 | `node.startup_x5` | quick | node 启动 x5 | 4.08s | 2.70s | 1.71s | **2.39×** | 1.51× |
| 8 | `node.json` | quick | node JSON | 1.72s | 1.04s | 0.49s | **3.51×** | 1.65× |
| 9 | `node.agent` | quick | node Agent 提示词 | 1.69s | 1.52s | 0.69s | **2.45×** | 1.11× |
| 10 | `node.text` | quick | node 日志/哈希/压缩 | 4.88s | 7.34s | 2.36s | **2.07×** | 0.66× |
| 11 | `family.py_startup` | quick | 旧版 python 启动 x10 | 2.85s | 2.26s | 1.54s | **1.85×** | 1.26× |
| 12 | `family.zlib` | quick | 旧版 zlib | 0.43s | 0.77s | 0.37s | **1.16×** | 0.56× |
| 13 | `family.bb_pipeline` | quick | 旧版 busybox 管道 | 0.96s | 1.24s | 0.49s | **1.96×** | 0.77× |
| 14 | `family.ash_loop` | quick | 旧版 ash 循环 | 1.03s | 2.77s | 0.66s | **1.56×** | 0.37× |
| 15 | `bb.all` | full | busybox 全家桶(录制负载) | 188.56s | 136.04s | 60.54s | **3.11×** | 1.39× |
| 16 | `node.fs` | full | node 遍历文件 | 0.90s | 0.63s | 0.65s | **1.38×** | 1.43× |
| 17 | `node.exec` | full | node 调子进程 | 2.65s | 1.10s | 0.78s | **3.40×** | 2.41× |
| 18 | `node.holdout` | full | node 未录制负载 | 3.05s | 6.06s | 2.15s | **1.42×** | 0.50× |
| 19 | `shell.pipeline` | full | shell 文本管道 | 16.19s | 45.10s | 15.39s | **1.05×** | 0.36× |
| 20 | `shell.bzip2` | full | bzip2 8MB | 2.01s | 5.70s | 5.60s | **0.36×** | 0.35× |
| 21 | `shell.node_churn` | full | node 对象/JSON 翻腾 | 5.54s | 9.07s | 3.35s | **1.65×** | 0.61× |
| 22 | `shell.selfmod` | full | 自修改代码 | 不适用：用例写入并执行 ARM64 机器码 | 0.44s | 0.18s | — | — |
| 23 | `py.t01_startup` | full | agent: python 启动 x20 | 4.43s | 2.39s | 1.29s | **3.43×** | 1.85× |
| 24 | `py.t02_json_stdin` | full | agent: stdin JSON x20 | 8.05s | 4.58s | 2.29s | **3.52×** | 1.76× |
| 25 | `py.t03_json_key` | full | agent: 读配置 x20 | 7.87s | 4.57s | 2.29s | **3.44×** | 1.72× |
| 26 | `py.t04_json_big` | full | agent: 大 JSON | 0.61s | 1.35s | 0.44s | **1.39×** | 0.45× |
| 27 | `py.t05_log_scan` | full | agent: 27MB 日志正则 | 3.74s | 14.29s | 2.99s | **1.25×** | 0.26× |
| 28 | `py.t06_base64` | full | agent: base64+JSON x5 | 1.51s | 1.52s | 0.93s | **1.62×** | 0.99× |
| 29 | `py.t07_pillow` | full | agent: Pillow 缩略图 | 2.30s | 2.60s | 1.78s | **1.29×** | 0.88× |
| 30 | `py.t08_jwt` | full | agent: ES256 JWT x100 | 1.36s | 1.50s | 0.67s | **2.03×** | 0.91× |
| 31 | `py.t09_import_net` | full | agent: import requests/httpx | 5.31s | 6.45s | 2.59s | **2.05×** | 0.82× |
| 32 | `py.t10_bs4` | full | agent: BeautifulSoup | 11.90s | 24.06s | 6.21s | **1.92×** | 0.49× |
| 33 | `py.t11_sqlite` | full | agent: sqlite 聚合 | 0.57s | 1.10s | 0.49s | **1.16×** | 0.52× |
| 34 | `charts.pandas` | full | pandas 20万行 | 7.63s | 8.30s | 4.54s | **1.68×** | 0.92× |
| 35 | `charts.matplotlib` | full | matplotlib 4 张图 | 11.53s | 18.98s | 8.39s | **1.37×** | 0.61× |
| 36 | `charts.mpl_big` | full | matplotlib 20万点 JSON | 15.98s | 31.88s | 11.23s | **1.42×** | 0.50× |
| 37 | `pil.batch` | full | Pillow 批处理 | 4.46s | 4.09s | 2.10s | **2.12×** | 1.09× |
| 38 | `doc.pdf` | full | PDF 生成(reportlab+mpl) | 16.36s | 24.42s | 9.49s | **1.72×** | 0.67× |
| 39 | `doc.ppt` | full | PPT 生成(python-pptx) | 14.11s | 11.88s | 4.91s | **2.87×** | 1.19× |
| 40 | `net.tls` | full | TLS 握手+传输+hash | 6.98s | 6.37s | 2.49s | **2.80×** | 1.10× |
| 41 | `net.requests_cold` | full | requests 冷启动 x5 | 18.35s | 8.69s | 3.56s | **5.15×** | 2.11× |
| 42 | `net.sqlite` | full | SQLite 8千行+60轮 | 5.00s | 9.68s | 2.90s | **1.72×** | 0.52× |
| 43 | `net.node_https` | full | node https(未录制) | 6.89s | 3.54s | 1.97s | **3.50×** | 1.95× |
| 44 | `net.node_sqlite` | full | node:sqlite(未录制) | 6.94s | 15.88s | 4.66s | **1.49×** | 0.44× |

## 解读

- **差距集中在“反复冷启动进程”的用例**：python/node 启动 ×20、requests 冷启动、读配置/stdin JSON ×20、node 调子进程、busybox 全家桶。
  每个新进程都要重新翻译 libpython / node / musl 的冷代码，而 Mac 的 AOJIT 镜像正是把这部分预编译掉了（这些用例在 Mac 上 on/off 差 1.8–2.5×）。
  Windows 上同一个“python 启动 ×20”：xlat 4.09s、旧模板拼接（ISH_XLAT=0）3.58s、纯 gadget 5.18s —— xlat 的翻译开销比模板拼接还高，冷代码多的场景吃亏。
- **长时间计算型用例 Windows 已追平或超过 Mac AOJIT on**：bzip2、shell 文本管道、27MB 日志正则、sqlite、busybox 管道、zlib。热代码一旦翻译完成，xlat 生成的代码质量不输 arm64 JIT + 镜像。
- 因此要进一步缩小差距，方向是降低冷代码成本：翻译结果跨进程复用（同一 ELF 模块的块缓存）、热度阈值（冷块先走模板/gadget）、或做 x86_64 版的录制镜像。

## 注意事项

- 两边硬件不同（i5-13600K vs Apple Silicon），Mac 基线是 9/26 的 822 构建；倍数反映“整机 + ish”的差距，不是纯引擎差距。
- `shell.selfmod` 不适用：该用例在 RWX 内存中写入并执行 **ARM64 机器码**，x86_64 guest 上必然 SIGSEGV（-11）。
- `family.*` 用例：Mac 上加载旧版本 libz/libpython/busybox 以测镜像的家族匹配；x86_64 没有对应旧版本，`oldlib` 用当前版本代替，只有速度意义。
- `bb.all` 在 Windows 上退出码为 1：xlat 与纯 gadget 构建同样是 1，输出除 `ls -la` 时间戳外逐字一致，属工作负载本身（某个 grep 无匹配），耗时可比。
- `net.requests_cold` 波动大（同一构建两轮 18.4s / 30.8s，有网络因素）。
- ssh.* / gh.* 用例不在 Mac 基线中（且需要 AOJIT_SSH / gh），未计入。
- 本次对比过程中发现并修复了一个内存泄漏（957094fe：每个 guest 线程的翻译缓冲区退出时不释放，管道类负载在 4GB 机器上触发内存刹车，并拖慢后续用例）；修复前的那轮数据已全部作废，表中均为修复后的测量。

## 复现

```sh
# 主机侧：把 guest 套件放进 x86_64 rootfs 的 /tmp/aojit，补装 needs 中的包（apk + pip python-pptx==1.0.2），再生成输入
ish -f <rootfs> /bin/sh /tmp/aojit/setup.sh
# 每个用例单独起一个 ish（4GB 主机上整轮共用一个进程会因内存累积失败）
ish -f <rootfs> /usr/bin/python3 /tmp/aojit/run_cases.py -c '^py\.pip_list$' --modes cur -r 2
```


---

# 热启动 / 热代码对比（Warm/Hot Workload Comparison）

上面 44 个用例里，差距最大的都是反复新起进程、每次都要重新翻译冷代码的场景。这一节把首次翻译的开销排除掉，只比热代码。

## 方法

- 脚本：`benchmark/aojit/guest/warm/warm.py`、`warm.js`，两边完全相同。
- **进程内的用例（Python、Node.js）**：每个用例先跑 1 次预热，让 JIT 或 xlat 把用到的代码都翻译完，再在同一进程内跑 N 次。表中数字是每次的平均耗时。
- **Shell 和 C**：每次都是新进程，而 ish 的翻译不跨进程复用，所以各跑 3 次，取第 2、3 次中较好的一次。这些负载单次就要数秒，一次翻译的成本已经摊薄：首次和后续两次的耗时相差不到 2%。
- 实现统一：Shell 的 awk、sed、sort、bc 一律调用 `busybox <applet>`，因为 Windows 的 rootfs 装了 coreutils 和 GNU bc，实现不同不能比。C 用官方 EEMBC CoreMark，两边都是 `-O2 -static`。
- 各列的含义：
  - **Windows xlat**：`port/windows` 957094fe 的 `build-x86_64-jit`，Windows 11，i5-13600K。
  - **Mac AOJIT on / off**：`build-arm64-ipa822`，也就是 44 用例那份 822 基线用的构建。它是只跑 AOT 镜像、不分配可执行内存的 iOS 形态，所以 off 等于纯 gadget 解释执行。
  - **Mac arm64 JIT**：`build-arm64-jit`（feat/aot-family-match，运行时生成机器码），作为“两边都是 JIT”的参照。它的代码区只有 120MB，Python 部分会把它写满，之后的进程就只能退回 gadget，所以这一列把 Python、Shell/C、Node 拆成三次独立的 ish 启动来跑。
- 倍数 = Windows 耗时 ÷ Mac 耗时，大于 1 表示 Windows 更慢。CoreMark 是分数，已经换算成同一方向。

## 逐项对照

| 组 | 用例 | Windows xlat | Mac AOJIT on | Mac AOJIT off | Mac arm64 JIT | Win/Mac-on | Win/Mac-off | Win/Mac-JIT |
|---|---|---:|---:|---:|---:|---:|---:|---:|
| Python | json dumps+loads x1000 | 6.244 ms/次 | 6.059 ms/次 | 21.3 ms/次 | 2.487 ms/次 | **1.03×** | 0.29× | 2.51× |
| Python | regex scan x50 | 39.0 ms/次 | 32.6 ms/次 | 88.0 ms/次 | 15.7 ms/次 | **1.20×** | 0.44× | 2.48× |
| Python | sqlite batch x20 | 112.3 ms/次 | 64.5 ms/次 | 230.3 ms/次 | 35.4 ms/次 | **1.74×** | 0.49× | 3.17× |
| Python | fib(22) x20 | 29.8 ms/次 | 10.5 ms/次 | 37.3 ms/次 | 8.388 ms/次 | **2.82×** | 0.80× | 3.55× |
| Python | prime sieve 200K x20 | 7.941 ms/次 | 9.919 ms/次 | 19.3 ms/次 | 3.575 ms/次 | **0.80×** | 0.41× | 2.22× |
| Python | nbody 1000 steps x10 | 91.0 ms/次 | 36.9 ms/次 | 112.3 ms/次 | 24.8 ms/次 | **2.46×** | 0.81× | 3.66× |
| Python | Pillow resize+blur x20 | 42.2 ms/次 | 20.6 ms/次 | 69.1 ms/次 | 16.2 ms/次 | **2.04×** | 0.61× | 2.61× |
| Python | pandas groupby+rolling x20 | 82.4 ms/次 | 32.2 ms/次 | 46.6 ms/次 | 25.2 ms/次 | **2.56×** | 1.77× | 3.27× |
| Shell | awk loop 3M | 15,873.3 ms | 6,184.5 ms | 20,847.6 ms | 4,673.8 ms | **2.57×** | 0.76× | 3.40× |
| Shell | awk csv aggregate 200K | 2,003.9 ms | 798.4 ms | 3,688.8 ms | 539.2 ms | **2.51×** | 0.54× | 3.72× |
| Shell | sed regex 200K lines | 3,012.3 ms | 1,839.6 ms | 3,617.3 ms | 646.5 ms | **1.64×** | 0.83× | 4.66× |
| Shell | sort 200K lines | 16,989.7 ms | 10,994.5 ms | 40,473.7 ms | 7,640.9 ms | **1.55×** | 0.42× | 2.22× |
| Shell | bc pi 1500 digits | 6,356.4 ms | 10,525.7 ms | 13,209.6 ms | 3,028.2 ms | **0.60×** | 0.48× | 2.10× |
| C | CoreMark (iter/s) | 12,918 it/s | 3,112 it/s | 3,149 it/s | 16,372 it/s | **0.24×** | 0.24× | 1.27× |
| Node.js | json stringify+parse x10000 | 0.204 ms/次 | 0.143 ms/次 | 0.533 ms/次 | 0.088 ms/次 | **1.43×** | 0.38× | 2.32× |
| Node.js | sha256 64KB x2000 | 3.348 ms/次 | 0.816 ms/次 | 1.875 ms/次 | 0.800 ms/次 | **4.10×** | 1.79× | 4.18× |
| Node.js | regex scan x50 | 4.222 ms/次 | 7.724 ms/次 | 31.8 ms/次 | 5.790 ms/次 | **0.55×** | 0.13× | 0.73× |
| Node.js | array sort 100K x10 | 673.3 ms/次 | 401.9 ms/次 | 1,051.3 ms/次 | 286.0 ms/次 | **1.68×** | 0.64× | 2.35× |
| Node.js | int loop 10M x5 | 421.8 ms/次 | 1,041.9 ms/次 | 4,626.5 ms/次 | 816.1 ms/次 | **0.40×** | 0.09× | 0.52× |

## 分组汇总

| 组 | 用例数 | Win/Mac-on（几何平均） | Win/Mac-off | Win/Mac-JIT |
|---|---:|---:|---:|---:|
| Python | 8 | **1.68×** | 0.61× | 2.89× |
| Shell | 5 | **1.58×** | 0.59× | 3.07× |
| C | 1 | **0.24×** | 0.24× | 1.27× |
| Node.js | 5 | **1.17×** | 0.35× | 1.54× |
| **全部** | 19 | **1.36×** | 0.50× | 2.38× |

## 结论：排除冷启动后的真实相对效率

- **相对 Mac AOJIT on：Windows x86_64 慢 1.36 倍（几何平均）**，大约是 1 : 1.36。冷启动场景下这个数是 1.91 倍，44 用例测量时的倍数里约有一半是翻译开销。
  - Python 1.68 倍、Shell 1.58 倍、Node 1.17 倍。
  - CoreMark 上 Windows 反而快 4.2 倍，因为 822 构建没有 CoreMark 的镜像，Mac 那边跑的是 gadget。
- **相对 Mac arm64 真 JIT：Windows 慢 2.38 倍**（Python 2.89 倍、Shell 3.07 倍、Node 1.54 倍），其中 CoreMark 只差 **1.27 倍**（12918 vs 16372）。
- **相对 Mac AOJIT off：Windows 快约 2 倍**（0.50 倍）。
- 差距最大的用例：Node sha256 4.10 倍、Python fib 2.82 倍、awk 循环 2.57 倍、pandas 2.56 倍、awk 聚合 2.51 倍。
- 差距最小（Windows 更快）：CoreMark 0.24 倍、Node 整数循环 0.40 倍、Node 正则 0.55 倍、bc 0.60 倍、质数筛 0.80 倍。

## 解读

- **差距集中在解释器类负载**：CPython 的字节码循环、busybox awk、bc 解释执行。CoreMark 这种直线式 C 代码只差 1.27 倍，而 fib、nbody 这类纯字节码分派的用例差 3.5 倍以上。
  - 推测原因（待用 profile 证实）：解释器每条字节码都靠间接跳转（`jmp *reg`）分派。xlat 对间接跳转不做原生翻译，而是退回 gadget，再走 `fiber_ret_lookup` 查块缓存。arm64 JIT 在这方面的处理更轻。
  - 因此下一步最值得做的是在原生代码里内联处理间接跳转和 ret：同一跳转点缓存上次的目标、直接比较后跳转。
- **Node 组不完全可比**：Mac 上的 arm64 ish 默认给 node 注入了 `--no-opt --no-maglev --no-sparkplug --regexp-interpret-all` 等参数，JS 只走解释器；Windows 的 x86_64 node 跑的是完整 V8。所以 Node 的整数循环、正则两项 Windows 占了引擎模式的便宜。JSON、sha256、sort 主要在 C++ 内建函数里，相对可比。
- **硬件不同**：i5-13600K 对 Apple Silicon。倍数是“整机 + ish”的差距；原生 CoreMark 在这台 Windows 上约 47790。

## 原始数据

Windows 与 Mac 各配置的逐行输出（组|用例|热耗时 ms|总耗时 ms|首次耗时 ms）见 `benchmark/aojit/guest/warm/` 的脚本说明；复现方法：

```sh
# Windows（x86_64 rootfs，补装 python3/nodejs/py3-pillow/py3-pandas）
ish -f <rootfs> /bin/sh -c 'cd /tmp/aojit/warm && python3 warm.py <官方 CoreMark>; node warm.js' </dev/null
# Mac 822：echo on|off > /proc/ish/jit 后同上；arm64 JIT 用 WARM_ONLY=py / WARM_ONLY=proc / node 分三次启动
# 注意 stdin 要接 /dev/null：822 构建没有忽略 SIGTTOU，node 退出时会把后台进程组里的 ish 停住
```
