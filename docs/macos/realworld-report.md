# 階段四：真實應用場景（macOS ish VM／JIT vs docker）

分支 explore/mac-jit-limits（worktree `~/Src/github.com/ish-arm64-macjit`）。本報告隨進度增量更新。

## 0. 階段三收尾

- 結束時：CoreMark 1t 25126 it/s（docker 44863 → **0.56×**），M1（0.6×）未達。
- fastmem 提交狀態見 §0.1。

### 0.1 fastmem 提交前驗證（6e80ba87）

- 回歸套件 SBA=1、SBA=1 FASTMEM=0、SBA=0 全部通過；新增 regress_fastmem_rights。
- iOS 物件 94/94 與 d511e83c 一致。
- 沒有殘留的 ISH_TMP 開關。
- 23 個 macjit case 與 7bb07a6a 比，md5 全部和 docker 一致，時間幾何平均 0.888。
  - fork_exec 慢 15%（0.261→0.300 s）；
  - find 慢 20%（0.073→0.087 s）。
- 相容性套件（A/B 背靠背，主機當時在 swap）：7bb07a6a 206/240，fastmem 210/240。
  - 只有一方失敗的都是逾時，兩個方向都有（3 對 7）；
  - 重試時只有 `import os` 不同，而且是 7bb07a6a 連續兩次失敗。
- 已提交：6e80ba87 fastmem、fa4f423b 工具。

### 0.2 「寫入夾在空洞中間」不報錯：與 fastmem 無關的既有行為

測試程式 `/tmp/cm/hole.c` 的流程：
1. 在一個 16K 對齊的 8 頁匿名區塊上，先對第 3 頁跑 2000 次熱寫入；
2. 再 `munmap` 第 3 頁；
3. 接著寫入 200 次，統計 SIGSEGV 次數。

| 版本 / 設定 | 寫入出錯次數 |
|---|---|
| ish fm5（fastmem） | 0 / 200 |
| ish fm5 `ISH_FASTMEM=0` | 0 / 200 |
| ish fm5 `ISH_JIT=0` | 0 / 200 |
| ish flagfix（7bb07a6a，fastmem 之前） | 0 / 200 |
| ish flagfix `ISH_JIT=0` | 0 / 200 |
| ish 3a9fdab6（更早） | 0 / 200 |
| docker（Linux） | 200 / 200 |

結論：ish 原本就有「寫入相鄰空洞會自動補頁」的行為（kernel/calls.c 的缺頁策略）。所以 `regress_fastmem_rights.c` 只檢查 PROT_NONE 頁與唯讀頁的寫入。這是 ish 與 Linux 的既有語意差異，另列為待確認事項。

## 1. 環境與輸入

- **rootfs**：`~/Src/github.com/ish-arm64/alpine-arm64-rw`
  - 由 alpine-arm64-321-latest 經 APFS clone 而來，放在內接 SSD，讓 I/O 場景公平；
  - 在 docker 裡 chroot 進去安裝：git、gcc、musl-dev、make、jq、sqlite、curl、linux-headers、python3-dev；
  - pip 安裝：pytest、flask、requests、jinja2、click、black、ruff；
  - npm：express、fastify、jest、typescript、eslint、prettier、esbuild，共 483 個套件，快取在 /root/.npm；
  - 素材：curl 倉庫（bare，40271 個 commit，137MB）、lua 5.4.7、sqlite 3.49.1 amalgamation；
  - 資料由 `benchmark/macjit/rw/gen_rw.py` 以固定種子產生；檔案樹約 1.1GB、40390 個檔案。
- **docker**：image `macjit-rw` 由同一份 rootfs 匯入，版本完全相同（python 3.12.14、node 22.23.2、npm 10.9.1、git 2.47.3）。
- **與 -latest 的差異**：-latest 的 npm 被 Minis 換成 npm-safe 包裝腳本，在 docker 裡跑不起來。這份 rootfs 還原成原版 npm-cli.js，git 加上 `safe.directory=*`。
- **執行器**：`benchmark/macjit/rw/rw.py`（在環境內執行）與 `rwrun.py`（主機端；每個場景都起全新的環境：第 1 次為冷啟動，其後 3 次熱啟動取中位數）。每個場景都校驗輸出的 md5 或測試通過數。

## 2. 場景

### Python

| 代號 | 場景 | 內容 |
|---|---|---|
| P1 | py_c_pass | `python3 -c pass` ×5 |
| P1 | py_json_tool | `python3 -m json.tool`，2MB |
| P1 | pip_version / pip_list | — |
| P2 | pip_install | 新建 venv，從本地 wheel 離線安裝 requests、jinja2、click、pyyaml、rich；含 markupsafe、pyyaml、charset_normalizer 三個 musl C 擴充 |
| P3 | pytest | 400 個用例，含 fixture 與 parametrize |
| P4 | py_csv | 500k 行 CSV |
| P4 | py_json | 150k 筆 JSON |
| P4 | py_sqlite | 插入 200k 筆並查詢 2000 次 |
| P4 | py_re_log | 300k 行日誌的正則解析 |
| P5 | flask_5k | Flask 開發伺服器；從啟動到首次回應，加上 5000 次 keep-alive 請求 |
| P6 | ruff / black | 掃描 stdlib 的 10 個套件，2.6MB |

### Node

| 代號 | 場景 | 內容 |
|---|---|---|
| N1 | node_e0 | `node -e 0` ×5 |
| N1 | npm_version | — |
| N1 | npx_local | 命中本地快取 |
| N2 | npm_ci | 離線 `npm ci`，483 個套件 |
| N3 | tsc | 120 個模組的 TS 專案 |
| N3 | esbuild | bundle |
| N4 | jest / node_test | 各 200 個用例 |
| N5 | express_5k | — |
| N6 | eslint | 掃描 express/lib 與 fastify/lib |
| N6 | prettier | `--check` |

### Shell

| 代號 | 場景 | 內容 |
|---|---|---|
| S1 | git_clone | 從本地 bare 倉庫 clone curl |
| S1 | git_ops | status、log（40k 個 commit）、diff --stat、checkout |
| S2 | make_lua | — |
| S2 | gcc_sqlite | `gcc -O1` 編譯 sqlite3.c |
| S3 | find_tree / grep_tree / tar_gz_tree | 1.1GB 檔案樹 |
| S3 | sort_log | 300k 行 |
| S4 | loop_awk_sed | for 迴圈加 awk/sed 管道 |
| S4 | jq | 100k 行 JSONL |

### 替代與略過

- N3 的 webpack/vite 沒有做，以 esbuild bundle 代替：vite 依賴 rollup 的原生 musl 套件，webpack 太重，兩者都偏離「中型」。
- P5 用 Flask 開發伺服器（單執行緒），沒有用 uvicorn。
- 客戶端是同一環境裡的 Python http.client；本機沒有 ab/wrk。
- S3 的檔案樹是 1.1GB（下限），不是 2GB。

## 3. 完整耗時表（最終：docker / VM / JIT 同時段交替）

- 版本：VM = 43956e9a（HVF 預設開），JIT = 同一個執行檔加 `ISH_HVF=0`。jest、node_test 兩列用 2cd8d853 重跑（`05b-jest`）：43956e9a 在 vCPU 用完時讓退回 JIT 的執行緒用到沒建好的 fastmem 別名，jest 4/4 失敗、node_test 只過 160/200；修正後兩者全過，三方一起重跑。
- iOS＋AOT、iOS gadget 兩欄是 d511e83c 照 iOS Release 參數（debugoptimized、`jit_emit=false`）在 macOS 上建的 CLI，AOT 鏡像是 Minis 出貨的 22 個（`02-ios-compare`，另一時段）。
- rootfs `alpine-arm64-rw` 以 realfs（`-r`）掛載；每個場景冷 1 次＋熱 3 次，表中是熱啟動中位數（秒）；負載 < 10 才跑。
- 結果目錄：`/Volumes/External HD/ish-work/macjit/results/20261027rw/{05-final,05b-jest,02-ios-compare}`；表由 `/tmp/cm/final_table.py` 產生。

### 3.1 逐場景

| 場景 | 類別 | docker | VM | JIT | iOS+AOT | iOS gadget | VM/docker | JIT/docker | VM ≤2× | 輸出 |
|---|---|---|---|---|---|---|---|---|---|---|
| py_c_pass | python | 0.029 | 0.059 | 0.206 | 0.269 | 0.647 | 2.04× | 7.14× | ❌ | docker 一致 |
| py_json_tool | python | 0.040 | 0.092 | 0.228 | 0.366 | 1.529 | 2.30× | 5.70× | ❌ | docker 一致 |
| pip_version | python | 0.084 | 0.192 | 0.487 | 0.746 | 2.213 | 2.28× | 5.78× | ❌ | docker 一致 |
| pip_list | python | 0.106 | 0.221 | 0.623 | 0.964 | 3.095 | 2.09× | 5.89× | ❌ | docker 一致 |
| pip_install | python | 2.349 | 4.165 | 11.037 | 23.393 | 66.781 | 1.77× | 4.70× | ✅ | docker 一致 |
| pytest | python | 0.220 | 0.644 | 2.061 | 3.737 | 12.908 | 2.92× | 9.35× | ❌ | docker 一致 |
| py_csv | python | 0.604 | 0.723 | 4.353 | 10.595 | 49.908 | 1.20× | 7.20× | ✅ | docker 一致 |
| py_json | python | 0.573 | 1.012 | 2.060 | 3.526 | 15.652 | 1.77× | 3.59× | ✅ | docker 一致 |
| py_sqlite | python | 0.470 | 1.156 | 2.189 | 4.175 | 18.079 | 2.46× | 4.66× | ❌ | docker 一致 |
| py_re_log | python | 0.280 | 0.794 | 1.741 | 3.414 | 21.989 | 2.84× | 6.22× | ❌ | docker 一致 |
| flask_5k | python | 1.038 | 6.152 | 22.035 | 18.617 | 36.389 | 5.93× | 21.24× | ❌ | docker 一致（vm 1/4 失敗，jit 1/4 失敗：主機連接埠耗盡） |
| ruff | python | 0.029 | 0.119 | 0.319 | 0.582 | 0.577 | 4.07× | 10.91× | ❌ | docker 一致 |
| black | python | 6.364 | 9.747 | 38.724 | 68.598 | 200.214 | 1.53× | 6.09× | ✅ | docker 一致 |
| node_e0 | node | 0.130 | 0.529 | 0.850 | 1.065 | 1.471 | 4.07× | 6.53× | ❌ | docker 一致 |
| npm_version | node | 0.046 | 0.194 | 0.420 | 0.582 | 1.101 | 4.21× | 9.14× | ❌ | docker 一致 |
| npx_local | node | 0.156 | 0.610 | 1.405 | 2.119 | 4.781 | 3.92× | 9.03× | ❌ | docker 一致 |
| npm_ci | node | 1.168 | 2.076 | 7.228 | 14.033 | 45.294 | 1.78× | 6.19× | ✅ | docker 一致 |
| tsc | node | 0.624 | 1.151 | 5.403 | 10.353 | 42.099 | 1.84× | 8.66× | ✅ | docker 一致 |
| esbuild | node | 0.008 | 0.083 | 0.148 | 0.283 | 0.281 | 9.94× | 17.78× | ❌ | docker 一致 |
| jest | node | 0.841 | 3.724 | 7.377 | 8.869 | 17.968 | 4.43× | 8.77× | ❌ | docker 一致 |
| node_test | node | 0.135 | 0.691 | 1.640 | 2.030 | 3.392 | 5.11× | 12.13× | ❌ | docker 一致 |
| express_5k | node | 0.606 | 1.404 | 5.585 | 7.988 | 22.964 | 2.32× | 9.22× | ❌ | docker 一致 |
| eslint | node | 0.088 | 0.282 | 0.584 | 0.896 | 1.934 | 3.21× | 6.63× | ❌ | docker 一致 |
| prettier | node | 0.596 | 0.918 | 4.443 | 8.209 | 34.391 | 1.54× | 7.45× | ✅ | docker 一致 |
| git_clone | shell | 0.235 | 0.652 | 0.660 | 1.077 | 1.798 | 2.78× | 2.81× | ❌ | docker 一致 |
| git_ops | shell | 0.952 | 2.525 | 3.052 | 6.936 | 9.404 | 2.65× | 3.21× | ❌ | docker 一致 |
| make_lua | shell | 3.952 | 6.482 | 29.983 | 106.055 | 110.676 | 1.64× | 7.59× | ✅ | docker 一致 |
| gcc_sqlite | shell | 6.845 | 10.793 | 37.173 | 174.487 | 182.665 | 1.58× | 5.43× | ✅ | docker 一致 |
| find_tree | shell | 0.080 | 0.549 | 0.508 | 1.078 | 1.345 | 6.91× | 6.38× | ❌ | docker 一致 |
| grep_tree | shell | 11.012 | 18.000 | 45.242 | 100.815 | 278.030 | 1.63× | 4.11× | ✅ | docker 一致 |
| tar_gz_tree | shell | 3.102 | 3.133 | 5.222 | 10.044 | 29.880 | 1.01× | 1.68× | ✅ | docker 一致 |
| sort_log | shell | 6.696 | 6.608 | 23.086 | 97.581 | 129.509 | 0.99× | 3.45× | ✅ | docker 一致 |
| loop_awk_sed | shell | 0.943 | 1.141 | 3.091 | 6.134 | 24.025 | 1.21× | 3.28× | ✅ | docker 一致 |
| jq | shell | 0.295 | 0.308 | 0.904 | 6.917 | 13.064 | 1.04× | 3.06× | ✅ | docker 一致 |

flask_5k：每輪 5,000 條新連線，4 輪連跑會把 macOS 的臨時連接埠用完（`EADDRNOTAVAIL`，TIME_WAIT 30 s），VM、JIT 各有 1 次失敗；表中用成功的那幾次。

### 3.2 類別

| 類別 | VM/docker 中位數 | VM 幾何平均 | JIT/docker 中位數 | JIT 幾何平均 | VM ≤2× |
|---|---|---|---|---|---|
| python | 2.28× | 2.34× | 6.09× | 6.78× | 4/13 |
| node | 3.92× | 3.33× | 8.77× | 8.81× | 3/11 |
| shell | 1.61× | 1.75× | 3.36× | 3.76× | 7/10 |
| 全部 | 2.29× | 2.41× | 6.30× | 6.21× | 14/34 |

### 3.3 達到 docker 2 倍內（VM，14 個）

sort_log 0.99×、tar_gz_tree 1.01×、jq 1.04×、py_csv 1.20×、loop_awk_sed 1.21×、black 1.53×、prettier 1.54×、gcc_sqlite 1.58×、grep_tree 1.63×、make_lua 1.64×、py_json 1.77×、pip_install 1.77×、npm_ci 1.78×、tsc 1.84×

### 3.4 未達（VM，20 個）與剩餘瓶頸

CPU 已不是瓶頸（CoreMark = docker；穩態運算類全部 ≤ 1.8×）。剩下的是 iSH 核心本身：

| 場景 | VM/docker | 卡在哪 |
|---|---|---|
| py_c_pass、py_json_tool、pip_version、pip_list | 2.0–2.3× | 行程啟動：每次 python 約 12 ms（docker 6 ms）。exec/mmap 在 iSH 核心裡做，每次系統呼叫多一次離開 VM（約 1 µs），munmap 還要同步清 TLB |
| express_5k、flask_5k | 2.3×、5.9× | 系統呼叫密集（每請求一條新連線）；flask 另受主機連接埠限制 |
| py_sqlite、py_re_log、pytest | 2.5–2.9× | 檔案系統呼叫與行程啟動（pytest 收集大量檔案） |
| git_clone、git_ops | 2.7–2.8× | 宿主檔案系統呼叫（realfs，每個檔案多次 stat/open） |
| eslint、npx_local、node_e0、npm_version、node_test、jest | 3.2–5.1× | node 啟動（`node -e 0` 0.53 s vs docker 0.13 s）、閒置 node 佔 7 個 vCPU，jest 這類多 worker 的有一部分執行緒超過 64 個 vCPU 上限而退回 JIT |
| ruff、esbuild | 4.1×、9.9× | 絕對時間很短（docker 8–29 ms），主要是 ish 啟動與 exec 的固定成本 |
| find_tree | 6.9× | 宿主 `fstatat`（每檔約 11 µs，與 JIT 相同） |

### 3.5 早期基線（M1，JIT 6e80ba87，`00-baseline`）

以下是階段四一開始量的 JIT 基線，保留作對照。


- ish = 6e80ba87（fastmem），rootfs 以 `-r` 掛載（realfs）。
- 每個場景都在全新的環境裡跑：冷啟動 = 第 1 次（JIT 冷、主機快取熱），熱啟動 = 第 2–4 次的中位數。
- 負載 < 9 才跑。
- 輸出校驗 34/34 全部與 docker 一致。

量測過程中修正的環境問題：
- **pytest**：realfs 下檔案擁有者是主機 uid 501，pytest 的 tmp 擁有者檢查失敗（200 個 error）。改用 `--basetemp=/tmp/rwpt` 後重跑，兩邊都是 400 passed。
- **find_tree**：冷啟動第一次是 36 秒，因為 clone 出來的樹主機快取還冷；預熱後重跑。
- **black**：ish 沒有 /dev/shm，multiprocessing 不能用，black 退回單行程，要 150–169 秒。rootfs 加上 /dev/shm 後是 30.6 秒，見問題檔。

結果目錄：`/Volumes/External HD/ish-work/macjit/results/20261027rw/00-baseline`

| scenario | group | docker cold | docker hot | ish-fm cold | ish-fm hot | hot ratio | cold ratio | ≤2× | check |
|---|---|---|---|---|---|---|---|---|---|
| py_c_pass | python | 0.033 | 0.027 | 0.212 | 0.204 | 7.51× | 6.40× | ❌ | ok |
| py_json_tool | python | 0.058 | 0.039 | 0.293 | 0.226 | 5.74× | 5.07× | ❌ | ok |
| pip_version | python | 0.167 | 0.082 | 0.595 | 0.453 | 5.51× | 3.56× | ❌ | ok |
| pip_list | python | 0.132 | 0.099 | 0.635 | 0.590 | 5.96× | 4.79× | ❌ | ok |
| pip_install | python | 2.385 | 2.275 | 10.466 | 10.448 | 4.59× | 4.39× | ❌ | ok |
| py_csv | python | 0.626 | 0.602 | 4.413 | 4.366 | 7.25× | 7.04× | ❌ | ok |
| py_json | python | 0.613 | 0.543 | 2.062 | 2.015 | 3.71× | 3.37× | ❌ | ok |
| py_sqlite | python | 0.477 | 0.463 | 2.034 | 2.066 | 4.46× | 4.26× | ❌ | ok |
| py_re_log | python | 0.310 | 0.261 | 1.692 | 1.705 | 6.52× | 5.46× | ❌ | ok |
| flask_5k | python | 1.133 | 0.977 | 11.218 | 12.322 | 12.62× | 9.90× | ❌ | ok |
| ruff | python | 0.071 | 0.017 | 0.426 | 0.283 | 16.48× | 5.96× | ❌ | ok |
| node_e0 | node | 0.238 | 0.079 | 1.162 | 0.889 | 11.26× | 4.88× | ❌ | ok |
| npm_version | node | 0.115 | 0.050 | 0.700 | 0.500 | 10.07× | 6.09× | ❌ | ok |
| npx_local | node | 0.369 | 0.182 | 2.230 | 2.677 | 14.71× | 6.04× | ❌ | ok |
| npm_ci | node | 1.601 | 1.092 | 15.899 | 8.467 | 7.76× | 9.93× | ❌ | ok |
| tsc | node | 0.779 | 0.658 | 5.149 | 4.988 | 7.58× | 6.61× | ❌ | ok |
| esbuild | node | 0.029 | 0.008 | 0.210 | 0.146 | 17.42× | 7.13× | ❌ | ok |
| node_test | node | 0.222 | 0.116 | 1.704 | 1.341 | 11.58× | 7.67× | ❌ | ok |
| express_5k | node | 0.674 | 0.617 | 5.365 | 5.111 | 8.28× | 7.96× | ❌ | ok |
| eslint | node | 0.167 | 0.083 | 0.903 | 0.578 | 6.93× | 5.39× | ❌ | ok |
| prettier | node | 0.578 | 0.537 | 5.272 | 4.611 | 8.58× | 9.13× | ❌ | ok |
| git_clone | shell | 0.400 | 0.250 | 3.300 | 0.676 | 2.71× | 8.25× | ❌ | ok |
| git_ops | shell | 0.966 | 0.963 | 2.121 | 3.223 | 3.35× | 2.20× | ❌ | ok |
| make_lua | shell | 4.309 | 5.074 | 32.596 | 29.241 | 5.76× | 7.57× | ❌ | ok |
| gcc_sqlite | shell | 6.896 | 6.766 | 51.919 | 39.619 | 5.86× | 7.53× | ❌ | ok |
| tar_gz_tree | shell | 3.590 | 3.092 | 5.092 | 4.958 | 1.60× | 1.42× | ✅ | ok |
| sort_log | shell | 6.423 | 6.359 | 22.329 | 22.779 | 3.58× | 3.48× | ❌ | ok |
| loop_awk_sed | shell | 0.960 | 0.955 | 3.078 | 3.967 | 4.15× | 3.21× | ❌ | ok |
| jq | shell | 0.317 | 0.295 | 0.890 | 0.909 | 3.08× | 2.81× | ❌ | ok |
| pytest | python | 0.453 | 0.216 | 2.108 | 2.016 | 9.33× | 4.66× | ❌ | ok |
| find_tree | shell | 0.651 | 0.099 | 0.477 | 0.439 | 4.42× | 0.73× | ❌ | ok |
| grep_tree | shell | 14.159 | 10.989 | 43.398 | 44.625 | 4.06× | 3.06× | ❌ | ok |
| black | python | 3.682 | 3.707 | 30.554 | 31.237 | 8.43× | 8.30× | ❌ | ok |
| jest | node | 1.228 | 0.738 | 7.086 | 8.094 | 10.97× | 5.77× | ❌ | ok |

python: hot ratio geomean 6.92×, median 6.52×, 0/13 within 2×
node: hot ratio geomean 10.06×, median 10.07×, 0/11 within 2×
shell: hot ratio geomean 3.64×, median 3.81×, 1/10 within 2×


#### M1 時已達 50%（熱啟動 ≤ 2×）

- tar_gz_tree：1.60×

（find_tree 只有冷啟動是 0.73×，熱啟動 4.42×。）

#### M1 時未達（33 個）

- **Python 13 個**：中位數 6.5×
- **Node 11 個**：中位數 10.1×
- **Shell 9 個**：中位數 3.8×

## 4. 瓶頸拆帳（時間取樣）

> §4–§6 是 M1 時對 JIT（6e80ba87）的拆帳與建議，之後改走 HVF 虛擬機（§7），CPU 相關的部分已不適用；VM 下的剩餘瓶頸見 §3.4。

方法：每個場景在同一個 ish 裡循環執行，用 `sample` 取 5 秒，依 prof.py 的類別歸類忙碌樣本（%），「wait」是等待樣本的占比。原始檔在 `results/20261027rw/01-survey`。

| scenario | busy samples | wait | generated code | dispatch | compile:gadget | compile:jit | block lookup | tlb | invalidate | syscall:fs | syscall | mmap/munmap | fork | exec | exit/mm teardown | gadgets | signal | other |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| py_c_pass | 8687 | 55% | 22 | 22 | 7 | 3 | 1 | 0 | 1 | 4 | 6 | 8 | 0 | 2 | 10 | 0 | 0 | 14 |
| py_json_tool | 8213 | 64% | 38 | 9 | 2 | 1 | 0 | 1 | 0 | 21 | 5 | 4 | 0 | 1 | 6 | 1 | 0 | 11 |
| pip_version | 4994 | 75% | 58 | 10 | 2 | 1 | 0 | 1 | 0 | 6 | 6 | 3 | 0 | 0 | 2 | 1 | 0 | 9 |
| pip_list | 4545 | 77% | 64 | 8 | 1 | 0 | 0 | 1 | 0 | 6 | 5 | 3 | 0 | 0 | 2 | 2 | 0 | 7 |
| pip_install | 3819 | 75% | 79 | 4 | 0 | 0 | 0 | 1 | 0 | 7 | 6 | 0 | 0 | 0 | 0 | 1 | 0 | 2 |
| pytest | 3784 | 79% | 81 | 5 | 0 | 0 | 0 | 1 | 0 | 3 | 5 | 0 | 0 | 0 | 0 | 1 | 0 | 3 |
| py_csv | 3877 | 75% | 96 | 3 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 1 | 0 | 0 |
| py_json | 3923 | 75% | 86 | 3 | 0 | 0 | 0 | 1 | 0 | 0 | 5 | 1 | 0 | 0 | 0 | 1 | 0 | 3 |
| py_sqlite | 3829 | 75% | 75 | 4 | 0 | 0 | 0 | 2 | 0 | 14 | 1 | 0 | 0 | 0 | 0 | 3 | 0 | 0 |
| py_re_log | 3866 | 75% | 93 | 4 | 0 | 0 | 0 | 0 | 0 | 0 | 1 | 0 | 0 | 0 | 0 | 2 | 0 | 0 |
| flask_5k | 3566 | 77% | 58 | 2 | 0 | 0 | 0 | 2 | 0 | 1 | 37 | 0 | 0 | 0 | 0 | 1 | 0 | 0 |
| ruff | 15219 | 56% | 9 | 41 | 2 | 0 | 0 | 2 | 0 | 1 | 8 | 8 | 0 | 0 | 1 | 0 | 0 | 28 |
| black | 39704 | 33% | 94 | 2 | 0 | 0 | 0 | 1 | 0 | 0 | 1 | 0 | 0 | 0 | 0 | 1 | 0 | 1 |
| node_e0 | 6038 | 78% | 14 | 25 | 5 | 3 | 0 | 1 | 1 | 1 | 13 | 11 | 0 | 0 | 4 | 1 | 0 | 20 |
| npm_version | 6353 | 81% | 27 | 17 | 3 | 1 | 0 | 1 | 0 | 3 | 11 | 7 | 0 | 0 | 11 | 1 | 0 | 18 |
| npx_local | 5939 | 86% | 33 | 15 | 2 | 1 | 0 | 1 | 0 | 7 | 11 | 5 | 0 | 0 | 6 | 2 | 0 | 16 |
| npm_ci | 4295 | 89% | 59 | 6 | 0 | 0 | 0 | 1 | 0 | 24 | 4 | 1 | 0 | 0 | 0 | 2 | 0 | 5 |
| tsc | 4254 | 88% | 79 | 8 | 0 | 0 | 0 | 0 | 0 | 1 | 4 | 2 | 0 | 0 | 0 | 2 | 0 | 5 |
| esbuild | 7012 | 75% | 37 | 18 | 4 | 2 | 0 | 2 | 0 | 4 | 6 | 2 | 0 | 1 | 4 | 1 | 0 | 18 |
| jest | 16610 | 86% | 29 | 25 | 2 | 3 | 0 | 1 | 0 | 2 | 12 | 4 | 0 | 0 | 1 | 2 | 0 | 19 |
| node_test | 13982 | 86% | 23 | 34 | 3 | 3 | 0 | 1 | 0 | 1 | 8 | 5 | 0 | 0 | 1 | 2 | 0 | 19 |
| express_5k | 3884 | 89% | 85 | 3 | 0 | 0 | 0 | 0 | 0 | 1 | 5 | 0 | 0 | 0 | 0 | 2 | 0 | 2 |
| eslint | 5458 | 86% | 37 | 18 | 2 | 1 | 0 | 1 | 0 | 4 | 10 | 6 | 0 | 0 | 2 | 2 | 0 | 15 |
| prettier | 4207 | 90% | 75 | 7 | 0 | 0 | 0 | 0 | 0 | 1 | 8 | 2 | 0 | 0 | 0 | 2 | 0 | 5 |
| git_clone | 3992 | 75% | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 100 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 |
| git_ops | 5854 | 69% | 53 | 7 | 1 | 0 | 0 | 1 | 0 | 5 | 7 | 4 | 0 | 0 | 11 | 1 | 0 | 9 |
| make_lua | 5185 | 83% | 52 | 16 | 2 | 1 | 0 | 0 | 0 | 0 | 5 | 1 | 0 | 0 | 17 | 0 | 0 | 5 |
| gcc_sqlite | 3650 | 80% | 85 | 6 | 0 | 0 | 0 | 1 | 0 | 0 | 8 | 0 | 0 | 0 | 0 | 0 | 0 | 0 |
| find_tree | 3983 | 80% | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 100 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 |
| grep_tree | 3415 | 82% | 89 | 1 | 0 | 0 | 0 | 0 | 0 | 4 | 1 | 0 | 0 | 0 | 0 | 5 | 0 | 0 |
| tar_gz_tree | 4038 | 86% | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 99 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 |
| sort_log | 3924 | 80% | 97 | 1 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 2 | 0 | 0 |
| loop_awk_sed | 3814 | 86% | 90 | 3 | 0 | 0 | 0 | 0 | 0 | 1 | 1 | 0 | 0 | 0 | 0 | 2 | 0 | 2 |
| jq | 4859 | 77% | 81 | 4 | 0 | 0 | 0 | 0 | 0 | 0 | 2 | 2 | 0 | 0 | 6 | 1 | 0 | 3 |


（「other」主要是 libsystem 的 pthread、malloc，以及背景拆除執行緒的 munmap。）


## 5. 按類別的結論（把「卡在哪」說成數字）

### A. 啟動與多行程類

場景和熱啟動倍數：

| 場景 | 倍數 | 場景 | 倍數 |
|---|---|---|---|
| py_c_pass | 7.5× | ruff | 16.5× |
| pip_version | 5.5× | esbuild | 17.4× |
| pip_list | 6.0× | eslint | 6.9× |
| node_e0 | 11.3× | jest | 11.0× |
| npm_version | 10.1× | node_test | 11.6× |
| npx_local | 14.7× | | |

每次行程啟動的固定成本：

| | ish | docker |
|---|---|---|
| python3 -c pass | 40 ms | 5.4 ms |
| node -e 0 | 178 ms | 16 ms |
| ruff（Rust） | 283 ms | 17 ms |
| esbuild（Go） | 146 ms | 8 ms |

拆帳：

| 項目 | py_c_pass | node_e0 |
|---|---|---|
| 生成碼 | 22% | 14% |
| 派發 | 22% | 25% |
| 翻譯（gen + jit） | 10% | 8% |
| mmap/munmap | 8% | 11% |
| 行程結束與 mm 拆除 | 10% | 4% |
| 系統呼叫 | 10% | 14% |
| exec | 2% | — |
| 其它（libsystem） | 14% | 20% |

這類場景對應「每個行程重做 gen 與重新連結」那個槓桿（a），可吃到的部分 = 翻譯 + 派發裡第一次連結的成本，加上 mm 建立與拆除。

### B. 穩態解譯器運算

場景和熱啟動倍數：

| 場景 | 倍數 | 場景 | 倍數 |
|---|---|---|---|
| py_csv | 7.3× | black（已修 /dev/shm） | 8.4× |
| py_re_log | 6.5× | gcc_sqlite | 5.9× |
| py_sqlite | 4.5× | make_lua | 5.8× |
| py_json | 3.7× | sort_log | 3.6× |
| pytest | 9.3× | grep_tree | 4.1× |
| pip_install | 4.6× | loop_awk_sed | 4.2× |
| | | jq | 3.1× |

- 生成碼占 75–97%，是 JIT 碼本身的速度。
- 依階段二的帳本，主要的指令開銷是派發：直接邊每條約 8 條宿主指令，間接跳轉加 ret 每次約 30–40 條。Python 的 computed goto 每個 bytecode 都要付一次間接跳轉。
- fastmem 之後，訪存檢查已經不是大頭。

### C. Node 運算

場景和熱啟動倍數：express_5k 8.3×、tsc 7.6×、prettier 8.6×、npm_ci 7.8×（npm_ci 有 24% 在 fs）。

- 生成碼占 59–85%。
- 但 ish 注入的旗標讓 V8 只跑直譯器，同機原生下這本身就只有 0.46×（也就是至少 2.2×）。所以只優化 ish，node 的上限就在 2× 左右。
- 要到 2× 以內，必須放開 V8 的 JIT 層，也就是槓桿（b）。

### D. 檔案系統類

- find_tree 4.4×：忙碌樣本 99.6% 在宿主 `fstatat`。0.44 s 走 40k 個檔案，約每檔 11 µs。
- git_clone 2.7%：100% 在 fs 系統呼叫。
- git_ops 3.4×。
- tar_gz_tree 1.6× ✅。

這類場景對應槓桿（c）：realfs 的路徑解析與 stat 次數。

### E. 網路服務

- flask_5k 12.6×：37% 在系統呼叫。Flask 開發伺服器每個請求都 accept 一條新連線、做多次 poll/recv。
- express_5k 8.3×：生成碼 85%（Node 解譯器）。

### 環境缺口

- **/dev/shm**：black 從 150 s 降到 30.6 s，影響所有用 multiprocessing 的 Python 工具。
- **realfs 擁有者**：pytest 的 tmp 檢查。用 fakefs 時不會發生，但 fakefs 的 I/O 更重，I/O 類場景之後要用 fakefs 補量一組。

## 6. 建議的優先順序（待使用者排定）

1. **（a）跨行程共享 fiber block 與連結表**：涵蓋 A 類 11 個場景，以及 pip、npm、git、make、jest 這類「起一堆行程」的場景；可吃到的部分約 30–45%。
2. **（d）間接跳轉與 ret 的派發**：inline cache、熱路徑 superblock。B 類 13 個場景的主體都在這裡。
3. **（b）V8 JIT 碼掉回 run loop**：C 類的天花板由它決定。
4. **（c）realfs 的 stat 與路徑**，以及 flask 的網路系統呼叫：D、E 類。
5. **/dev/shm**：一行就能解決，但會改到 Minis/iOS 的 rootfs，所以要先由使用者決定。

## 7. 收尾：HVF 虛擬機後端、環境缺口與驗證

### 7.1 走向

使用者決定不再做 JIT 優化後，改用 Hypervisor.framework 讓 guest 程式碼在 VM 的 EL0 原生執行、iSH 繼續當核心（1e565e47，43956e9a 預設開）。CoreMark 1 執行緒 44,793 it/s、4 執行緒 170,712，等於 docker（44,543／170,667）。真實場景結果見 §3：VM / docker 中位數 **Python 2.28×、Node 3.92×、Shell 1.61×**（全部 2.29×），14/34 在 2 倍內。

（提交 1e565e47 的說明裡寫的 Python 2.40×、Node 3.34×、Shell 1.53× 是較早、非同時段的一輪（`04-hvf5`，VM 單獨跑），以本報告 §3 為準。）

依賴分支是 `integ/mac-hvf-stage2`：`explore/mac-hvf` 合入 `integ/stage2-on-depsish`（xnet 階段 2、ADR/ADRP 修正、fakefs hook 修正）與 MinisApp `deps/ish` 的 2f2626e7（ish-arm64#42），再加 40ff89d5（CLI 建 `/dev/shm`）、2cd8d853（`hvf_describe()`／`/proc/ish/hvf`；vCPU 用完時退回 JIT 的執行緒不再使用沒建好的 fastmem 別名）。接入方式見 `docs/macos/ISH_FOR_MAC.md`。

### 7.2 /dev/shm（之前掛著的問題）

- iSH 開機時沒有 `/dev/shm`：Minis 那種 fakefs rootfs 上，**VM 和 JIT 兩種模式都沒有**，Python `multiprocessing.Pool` 直接 `FileNotFoundError`。只要有這個目錄，兩種模式都能用。
- black `--check`（VM，realfs）：沒有 pool（`--workers 1`，等同缺 `/dev/shm`）32.3 s，有 pool 9.7 s，docker 4.6 s。
- 處理：iSH CLI 開機時建立（40ff89d5，只在 macOS）。App 走自己的開機流程（`ISHKernel.m` `bootWithRootPath:`），要在建 `/dev` 節點那段加同樣兩行（手冊 §3d）。建議由開機時建立而不是靠 rootfs 預建：既有使用者的 rootfs 不用補。

### 7.3 fakefs 下的 VM

| 場景 | docker | VM realfs | VM fakefs | fakefs/realfs | fakefs/docker | 輸出 |
|---|---|---|---|---|---|---|
| py_c_pass | 0.029 | 0.061 | 0.069 | 1.14× | 2.40× | 與 realfs 一致 |
| pip_list | 0.106 | 0.220 | 0.254 | 1.16× | 2.41× | 與 realfs 一致 |
| pytest | 0.220 | 0.627 | 1.375 | 2.19× | 6.24× | 與 realfs 一致 |
| py_csv | 0.604 | 0.754 | 0.796 | 1.06× | 1.32× | 與 realfs 一致 |
| node_e0 | 0.130 | 0.573 | 0.505 | 0.88× | 3.89× | 與 realfs 一致 |
| npx_local | 0.156 | 0.602 | 0.626 | 1.04× | 4.02× | 與 realfs 一致 |
| tsc | 0.624 | 1.125 | 1.450 | 1.29× | 2.32× | 與 realfs 一致 |
| git_ops | 0.952 | 2.448 | 14.725 | 6.01× | 15.47× | 與 realfs 一致 |
| make_lua | 3.952 | 6.658 | 7.264 | 1.09× | 1.84× | 與 realfs 一致 |
| sort_log | 6.696 | 6.421 | 6.392 | 1.00× | 0.95× | 與 realfs 一致 |
| jq | 0.295 | 0.317 | 0.321 | 1.01× | 1.08× | 與 realfs 一致 |

多數場景與 realfs 差 0.9–1.3×；git_ops 6.0×、pytest 2.2× 的差距包含磁碟差異（fakefs 根目錄在外接 USB 碟、realfs 在內建 SSD），在內建 SSD 上的倍數未驗證。結果：`06b-fakefs/`（2cd8d853）。

### 7.4 相容性、回歸、iOS

- 相容套件（同負載逐項 A/B）：43956e9a 上 JIT 240/240、VM 240/240（第一輪 VM 237/240：三個 npx 用例的 node 22.15.1 開 Sparkplug 就崩，原生 docker 也崩，VM 模式改為給 node 補 `--no-sparkplug` 後全過）；集成分支 2cd8d853 上 JIT 240/240、VM 240/240（`/tmp/cm/compat_integ.log`）。
- 回歸套件：2cd8d853，VM 與 `ISH_HVF=0` 都全過，含 xnet TCP 113、UDP/DNS 72（`/tmp/cm/regress_final_{vm,jit}.log`）。
- iOS：集成分支與 iOS 現在的樹（`integ/stage2-on-depsish` ⊕ 2f2626e7）以 iOS 參數、同一路徑建置，目標檔 94/94 一致。

### 7.5 浸泡

同一個 iSH 行程 40 分鐘混合負載（`07-soak/`）：190 輪全部正確；每輪耗時前 1/4 中位數 13 s、後 1/4 12 s，沒有漂移；每輪結束時 vCPU 一直是 13 個；宿主 RSS 在 55–370 MB 起伏，頭尾峰值相同。連續 500 個行程後位址空間 1 個在用（建了 1006 個）、vCPU 1 個在用（建了 503 個）：沒有 vCPU、ASID 洩漏。
