# iSH for Mac 接入手冊（給 MinisMac 主 worker）

> 讀者：在 MinisApp 做 Minis Mac（`src/macos`）的 worker。照本手冊做就能把新的 iSH 接進 MinisMac。
> 每個數字都追得到報告或日誌（§7）。沒驗證過的標「未驗證」，沒實測只是推論的標「推論」。

---

## 1. 概述與效果

**是什麼**：iSH 的 ARM64 Linux 核心照舊是 iSH（系統呼叫、檔案系統、行程、訊號都由 iSH 處理），但在 Mac 上，guest 程式碼不再翻譯，而是用 **Hypervisor.framework** 在虛擬機的 EL0 **原生執行**。iSH 以靜態庫 `libish.a` 跑在 App 行程內，一個 App 行程一台 VM。

**依賴分支**：`OpenMinis/ish-arm64` 的 **`integ/mac-hvf-stage2`**，程式碼固定在 **`2cd8d853`**（之後若有提交，只是 `docs/` 文件）。它是下面三條線的合併，**是 iOS 現在用的兩棵樹的超集**：

| 來源 | 內容 |
|---|---|
| `explore/mac-hvf`（43956e9a） | Mac 的 JIT 優化（SBA 窗口、fastmem…）＋ HVF 虛擬機後端（預設開） |
| `integ/stage2-on-depsish`（f0737196） | xnet 階段 2（UDP/DNS 透明轉發 ba1ee4da、TCP 選項 d144921f）、ADR/ADRP 64 位修正、fakefs hook 路徑修正 4 個 |
| MinisApp `deps/ish` 指針（2f2626e7） | 上面那些之外，再加 ish-arm64#42 長命令列修正（bc3a9aeb） |

另有本分支自己的 2 個程式碼提交：40ff89d5（CLI 建立 `/dev/shm`）、2cd8d853（引擎狀態查詢 `hvf_describe()`／`/proc/ish/hvf`，以及 vCPU 用完時 JIT 執行緒的 fastmem 修正），以及只加 `docs/macos/`、`tools/ios-objcmp/` 的文件提交。netfs（2cc68050 等）、UDP 多播（2769b2dd）、TCP 透明轉發（be1dc685）都在祖先裡。

**三種執行引擎與自動降級**（都在同一個 `libish.a` 裡，不需要 App 選）：

| 引擎 | 何時用 | 速度（相對 docker，34 個場景中位數） |
|---|---|---|
| **VM**（Hypervisor.framework，預設） | 執行檔有 `com.apple.security.hypervisor` 授權，且沒設 `ISH_HVF=0` | **2.29×** |
| **JIT**（執行期翻譯） | 沒有授權、`ISH_HVF=0`、建 VM 失敗；或個別執行緒拿不到 vCPU（整台 VM 超過 64 條） | 6.30× |
| gadget（直譯） | JIT 也拿不到可執行記憶體（沒有 `allow-jit`） | iOS gadget 版：慢 JIT 3–15 倍 |

降級是**靜默**的（只有明確設了 `ISH_HVF=1` 才會在 stderr 說為什麼沒有 VM），所以接完一定要用 §4.3 的方法確認引擎。

**效果摘要**（M4 Pro，3 輪熱啟動中位數，輸出全部與 docker 一致；完整表見 `docs/macos/realworld-report.md` §3）：

- CoreMark：VM 1 執行緒 44,793 it/s、4 執行緒 170,712，**等於 docker**（44,543／170,667）；JIT 是 24,691／84,779。
- 真實場景（Python 13、Node 11、Shell 10）VM / docker：**Python 2.28×、Node 3.92×、Shell 1.61×**，全部 2.29×；JIT 是 6.09×／8.77×／3.36×。VM 有 **14/34** 個場景在 docker 的 2 倍內（JIT 0/34）。jest、node_test 兩列用修正後的 2cd8d853 重跑（43956e9a 在超過 64 個 vCPU 時會讓退回 JIT 的執行緒出錯）。
  - 例：sort 0.99×、tar 1.01×、jq 1.04×、py_csv 1.20×、black 1.53×、prettier 1.54×、gcc 1.58×、tsc 1.84×、npm ci 1.78×。
  - 還慢的：esbuild 9.94×、find 6.91×、flask 5.93×、node --test 5.11×、jest 4.43×、node -e 0 4.07×。剩下的是 iSH 核心本身的成本（行程啟動、系統呼叫、宿主檔案系統），不在 CPU。

---

## 2. 依賴方式

### 2a. 新增子模組 `deps/ish-macos`

比照 Windows 的做法（`deps/ish-windows`，見 `src/windows/third_party/ish/README.md`）：**一個獨立子模組、同一個倉庫、不同分支**，主倉庫只記指針，和 `deps/ish`（iOS/Android，`compat/suite-more`）、`deps/ish-windows`（`port/windows`）互不干擾。

```bash
cd ~/Src/github.com/OpenMinis/MinisApp
git submodule add -b integ/mac-hvf-stage2 https://github.com/OpenMinis/ish-arm64.git deps/ish-macos
git -C deps/ish-macos checkout 2cd8d853          # 指針寫死
git -C deps/ish-macos submodule update --init deps/libapps   # 與 deps/ish 相同的子模組需求
git add .gitmodules deps/ish-macos
git commit -m "deps: add deps/ish-macos (integ/mac-hvf-stage2 @ 2cd8d853) for MinisMac"
```

`.gitmodules` 會多出：

```
[submodule "deps/ish-macos"]
	path = deps/ish-macos
	url = https://github.com/OpenMinis/ish-arm64.git
	branch = integ/mac-hvf-stage2
```

**為什麼不直接改 `deps/ish` 的指針**：`deps/ish` 是 iOS 和 Android 用的。`integ/mac-hvf-stage2` 比 iOS 的樹多了 Mac 的 22 個提交加 3 個合併與後續提交；改 `deps/ish` 會把 iOS/Android 一起帶動。雖然這些改動在 iOS 編譯下全部消失（見 §6，iOS 目標檔 94/94 與 iOS 現在的樹一致），但 Android 沒驗證過，而且 iOS 發版節奏不該被 Mac 綁住。

### 2b. 建置：`deps/build_ish_macos.sh`

現在的腳本預設從 `deps/ish` 建，也支援 `--src <dir>`。建議**把預設來源改成 `deps/ish-macos`**（一行）：

```bash
# deps/build_ish_macos.sh 第 81 行附近
ISH_DIR="$SCRIPT_DIR/ish-macos"     # 原本是 "$SCRIPT_DIR/ish"
```

並把第 26–32 行「Mac 必須用 `~/Src/github.com/ish-arm64-xnet`」的註解改成「Mac 用 `deps/ish-macos`（`integ/mac-hvf-stage2`，已含 stage 2）」。`record_source` 的 `nm ... T _ish_xnet_set_routes2` 檢查照留，它在新樹上會通過。

不想改預設的話，也可以每次用 `--src`：

```bash
deps/build_ish_macos.sh release --src deps/ish-macos
```

注意 `--src` 時建置目錄是 `deps/build/ish-macos-<目錄名>`（這裡是 `deps/build/ish-macos-ish-macos`），改預設則是 `deps/build/ish-macos`。

**產物不變**：

| 產物 | 位置 |
|---|---|
| `libish.a`、`libish_emu.a`、`libfakefs.a`（、`libish_aot.a`） | `deps/libs/macosx/` |
| 冒煙用 CLI `ish`、`SOURCE` | `deps/libs/macosx/` |
| 標頭 | `deps/include/macosx/ish/`（`kernel/` 整個目錄會被複製，新的 `kernel/hvf.h` 會跟著過去） |

**meson 參數照舊**：`-Djit=true -Djit_emit=true`（JIT 是 VM 不可用時的退路，必須保留）、`release`、`-Db_ndebug=true`、`-Dlog_handler=nslog`、native file 帶 `-mmacosx-version-min=14.0`。我用完全相同的參數在暫存目錄建過一次集成分支：設定和編譯都成功，`libish.a` 匯出 `hvf_on`、`hvf_describe`、`ish_xnet_set_routes2`（§7 [B1]）。Hypervisor.framework 是執行期 `dlopen` 的，**不用加任何連結參數或 framework**。

**`--no-aot` 的含義與建議**：`--no-aot` 跳過 `build_aot.sh`，不產生 `libish_aot.a`。AOT 鏡像對 Mac 沒有用處：VM 根本不翻譯；退回 JIT 時，Mac 的 JIT 程式碼 ABI（`JIT_CODE_VERSION` 14、含 SBA/fastmem 旗標）和 iOS 的鏡像不同，鏡像會被拒用（推論：ABI 檢查與 iOS 相同機制，未在 Mac 實測載入）。但 `project.pbxproj` 的 MinisMac 三個設定都寫死 `-force_load $(ISH_LIBS_DIR)/libish_aot.a`，**直接用 `--no-aot` 會連結失敗**。二選一：

- 保持現狀（不加 `--no-aot`），鏡像只是佔空間；或
- 在 pbxproj 的 MinisMac `OTHER_LDFLAGS` 拿掉 `-force_load $(ISH_LIBS_DIR)/libish_aot.a`，之後一律 `--no-aot`（App 會小約百 MB，推論）。

建置耗時：`src/macos/docs/DEPS_MAC.md` 記為約 1 分鐘（含 AOT）；純 iSH 部分在 M4 Pro 上是秒級到幾十秒。

### 2c. 與 xnet（透明網路）的關係——已解決

事實（我核對過）：

- `explore/mac-hvf` 只有 xnet 階段 1（be1dc685 TCP 轉發），**沒有**階段 2（ba1ee4da UDP/DNS、d144921f）。
- iOS 的 libish 從 `integ/stage2-on-depsish`（f0737196）建，Mac 現在也用這棵樹建（`deps/libs/macosx/SOURCE`）。
- MinisApp 的 `deps/ish` 指針 2f2626e7 與 f0737196 的 patch-id **相同**（同一個 ADR/ADRP 修正的兩個提交），但 `deps/ish` 線多一個 bc3a9aeb（ish#42），`integ/stage2-on-depsish` 沒有。

處理：我建了 **`integ/mac-hvf-stage2`** = `explore/mac-hvf` + 合併 `integ/stage2-on-depsish` + 合併 `2f2626e7`，已推送。驗證（§7 [V1]–[V4]）：

- 祖先檢查：f0737196、ba1ee4da、d144921f、bc3a9aeb、2191e9b3…e56de90a、2769b2dd、2cc68050、6269299d、be1dc685、43956e9a 全在。
- iOS 目標檔：與「`integ/stage2-on-depsish` ⊕ `2f2626e7`」這棵 iOS 樹在同一路徑建置後比對，**94/94 一致**。
- 回歸套件：VM 與 `ISH_HVF=0` 兩種模式全過，含 xnet TCP **113 項**、UDP/DNS **72 項**（74bb5b05 與最終的 2cd8d853 各跑一次）；另跑 `regress_long_cmdline.sh` 7/7、`regress_hook_paths.sh` 6/6（兩種模式）。
- 相容套件：2cd8d853 上 JIT 240/240、VM 240/240。

所以 **Mac 換到 `deps/ish-macos` 不會丟掉 stage 2**，也不缺 ADR/ADRP 和 #42 修正。

### 2d. 升級流程

**誰升級**：iSH 端的改動由 iSH worker 在 `integ/mac-hvf-stage2` 上做並推送；MinisMac worker 只移動指針。

**iSH 端（每次往這條分支合東西時）**：
1. 合入 `compat/suite-more`／`integ/stage2-on-depsish` 的新提交（或 Mac 專屬改動）。
2. iOS 零變化：用 `tools/ios-objcmp/`（`mkios.sh` 以 iOS Release 參數建、`-g0`、`neutral.h` 把 `__LINE__`/`__DATE__` 中性化；`cmp.sh` 逐檔比程式碼、資料、符號、重定位；步驟見該目錄 README）把變更後的樹和 iOS 當前的樹比，要 94/94。兩棵樹必須放在**同一路徑**先後建，否則 `__FILE__` 字串不同。
3. `tests/regress/run.sh -i <ish> -r <rootfs>` 兩種模式（預設、`ISH_HVF=0`）全過。
4. 相容套件 A/B（§5.2）兩種模式 240/240。
5. 推送。

**MinisMac 端**：
```bash
git -C deps/ish-macos fetch origin integ/mac-hvf-stage2
git -C deps/ish-macos checkout <新 commit>
deps/build_ish_macos.sh release            # 或 --src deps/ish-macos
# 建 MinisMac，跑 §5 的驗收
git commit -o deps/ish-macos -m "deps: ish-macos -> <新 commit>"
```

**回滾**：
- 原始碼層：`git -C deps/ish-macos checkout <舊 commit>`，重建，提交指針。
- 執行期一鍵退回 JIT（不用重建）：在第一次呼叫 iSH 之前 `setenv("ISH_HVF", "0", 1)`；或打包時拿掉 `com.apple.security.hypervisor` 授權。

---

## 3. 把 iSH 當成 shell execute 與 terminal 的沙盒

### 3a. 進程內 libish 的邊界

- **一個 App 行程一台 VM**（Hypervisor.framework 的限制）。`ISHKernel` 單例不變；第一次 exec（`bootWithRootPath:` 裡的 `do_execve`）時決定引擎。
- **每條命令是一個 guest 行程**，每個 guest 位址空間在 VM 裡有自己的頁表和 ASID（16 位，最多 65,535 個同時存在；用完的行程退回 JIT，實務上碰不到）。
- **每條 guest 執行緒佔一個 vCPU，整台 VM 最多 64 個**，執行緒結束才釋放（阻塞中也照佔）。實測佔用：`sh`、`python3` 各 1，閒置的 `node` 7（§7 [M2]）。所以**大約 8 個常駐 node（例如 MCP server）就會用滿**；之後新建的執行緒自動改跑 JIT（正確但較慢）。jest（11 個 worker）實測有 52 條執行緒退回 JIT，200 個測試全過（§7 [M3]）。
  - 風險：同一個 node 行程裡一部分執行緒在 VM、一部分在 JIT 時，若 VM 執行緒改寫了 V8 的 JIT 程式碼，JIT 執行緒可能還在跑舊的翻譯。jest、node --test 實測正常，但這是已知風險（§6）。
- **多會話／多終端**：iSH 這邊沒有「一次一個 shell」的限制，VM 也沒有。現在 Mac 終端「同一時刻一個 shell」來自 App 端：`ISHKernel.outputCallback` 是全域單一回呼、`sendInput`/`setTerminalSize` 寫的是單一 `_consoleTTY`（`ISHKernel.m:1872, 2028-2052`），`ISHTerminalSource` 用靜態 `owner` 擋第二個 tab。
  - **建議（App 端改，iSH 不用動）**：每個 tab 各開一個 `pty_open_fake(&ish_pty_driver)`，`ish_pty_driver` 的 write 回呼本來就帶 `struct tty *`；用 `tty → tab` 的表分流輸出，`sendInput`/`setTerminalSize` 帶上 tab 的 tty。這樣就能多 tab 各自一個 shell。未實作、未驗證。

### 3b. shell execute 的接線

**直接沿用 iOS 的 `ISHExecutionCoordinator` 路徑**（Mac target 已經編譯同一批檔案：`ISHExecutionCoordinator.swift`、`ISHShellExecutor.m`、`ISHKernel.m`、`MinisFsRouter.swift`），不要另寫。契約不變：

- `execute(sessionId:command:timeout:ownerId:lineCallback:pidCallback:)`；timeout `nil` = 300 s（chat 預設 900 s）；部分輸出鏡像保留**尾端 256,000 字元**；Stop 按 `ownerId` 過濾，SIGTERM 後 200 ms SIGKILL，3 s 後放棄等待。
- 命令經 `/bin/sh` 的 stdin 送入、`become_new_init_child` → `do_execve` → `task_start`。在 VM 裡每條命令就是一個 guest 行程，沒有額外成本。

**必須改的地方（只在 macOS）**：

1. **環境注入（`ISHShellExecutor.m:550-627` 與 `ISHKernel.m` `executeCommand` 1941-2004）**：App 自己注入了 `GOMAXPROCS=2`、`PYTHONMALLOC=malloc`、node 的 `LD_PRELOAD=/lib/zero_free.so`。iSH 的注入規則是「沒設才補」，所以 **App 的值會蓋掉 VM 模式的選擇**。在引擎是 VM 時拿掉這三個：
   ```objc
   #if TARGET_OS_OSX
   #include "kernel/hvf.h"
   bool vm = hvf_on();          // 第一次呼叫會決定引擎；在 boot 之後呼叫
   #endif
   // vm 為真時不要加：GOMAXPROCS=2、PYTHONMALLOC=malloc、node 的 LD_PRELOAD
   ```
   要保留：`GODEBUG=asyncpreemptoff=1`（見 3f）、`NO_COLOR=1`、`PYTHONDONTWRITEBYTECODE=1`、`TERM`/`HOME`/`PATH`/`LANG` 等。`OPENSSL_armcap=0`（只在 agent 路徑）會讓 OpenSSL 不用硬體加速指令；在 VM 裡可以拿掉（推論，未實測效益）。
2. **`/dev/shm`**：見 3d。
3. **當機處理器**：見 4b，**必做**。

### 3c. terminal

`src/macos/Terminal/ISHTerminalSource.swift` 不用為 VM 改任何東西：它走 `executeCommand(["/bin/sh","-l"])` → fake pty → 全域 `outputCallback`，這些都在 iSH 核心層，與執行引擎無關。要多 tab 才需要 3a 的 per-tab pty 改造。

注意（子代理調查時發現，與 VM 無關，未驗證）：路徑轉譯 hook 只在 `ensureKernelBooted` 裡安裝；若 Mac 終端先開機，`/var/minis/{offloads,attachments,workspace,browser}` 這四個按會話分的路徑可能沒有 hook。

### 3d. rootfs

- **安裝與位置**不變：`RootfsManager.installIfNeeded` 解壓 App 內的 `alpine-rootfs.zip` 到 `~/Library/Application Support/com.openminis.mac/Documents/alpine-rootfs/{data/, meta.db}`（沙盒版在 `~/Library/Containers/...`）。開發時可用 `MINIS_TERMINAL_ROOTFS=<dir>`。
- **fakefs vs realfs**：App 只用 fakefs（`mount_root(&fakefs, ...)`，`ISHKernel.m:1363`），請維持。我們的基準用 realfs（CLI `-r`）；fakefs 的 VM 數字見下表（同一個執行檔、同時段交替，§7 [F1]）：

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

- 11 個場景，同一個執行檔（2cd8d853，VM）、同一時段交替跑 realfs 與 fakefs，各冷 1 次＋熱 3 次，取熱中位數；docker 欄來自總表（`05-final`）。輸出全部與 realfs 一致。
- 大多數場景 fakefs 與 realfs 差 0.9–1.3×。**git_ops（6.0×）、pytest（2.2×）**這類大量建檔、改檔、stat 的場景差距大。**注意，這個差距還沒分離乾淨**：fakefs 根目錄（含 `meta.db`）放在外接 USB 碟，realfs 在內建 SSD，兩邊磁碟不同；Minis 的 rootfs 在內建 SSD。fakefs 每次中繼資料操作都要寫 SQLite，本來就比 realfs 貴，但在內建 SSD 上的實際倍數：**未驗證**。
- 這份 fakefs rootfs 由 `alpine-arm64-rw` 去掉 1.1 GB 測資樹後用 `tools/fakefsify` 轉成，檔案擁有者是宿主 uid 501（Minis 的是 root）。

- **`/dev/shm` 一定要建**：Minis 的 fakefs rootfs 沒有 `/dev/shm`，VM 和 JIT 兩種模式下 Python 的 `multiprocessing` 都直接 `FileNotFoundError`；有了以後都能用。black 在 VM 下：沒有 pool 時 32.3 s、有 pool 9.7 s（docker 4.6 s）（§7 [S1]）。
  - iSH CLI 已在開機時建立（40ff89d5）。**App 有自己的開機流程，要在 `bootWithRootPath:` 建 `/dev` 裝置節點那一段（步驟 7，`FsApplyOverlay()` 附近）加上**：
    ```objc
    #if TARGET_OS_OSX
        generic_mkdirat(AT_PWD, "/dev/shm", 01777);
        struct attr shm_mode = {.type = attr_mode, .mode = S_IFDIR | 01777};
        generic_setattrat(AT_PWD, "/dev/shm", shm_mode, true);
    #endif
    ```
    （與 `xX_main_Xx.h` 的寫法相同。）Linux 的 `/dev/shm` 是 tmpfs，這裡是 rootfs 裡的普通目錄，當掉的程式留下的檔案會保留；需要的話開機時清空。
  - 也可以改由 rootfs 預建（`RootfsPatch`），但每個既有使用者的 rootfs 都要補，不如開機時建。只改 macOS 路徑。
- **rootfs 裡的 node 22.15.1**：它開 V8 的 Sparkplug 編譯器就會崩（`new FileHandle()` 裡 V8 fatal，然後卡住），**在原生 docker 裡也一樣**（§7 [N1]）。iSH 在 VM 模式下會自動給 node 補 `--no-sparkplug`（代價約 6%），App 不用處理。以後 rootfs 換新版 node 可以重新評估。

### 3e. 掛載與路徑

都是 iSH 核心（fakefs）層的機制，VM 不影響，照現狀：

| guest 路徑 | 宿主 | 機制 |
|---|---|---|
| `/var/minis/{offloads,attachments,workspace,browser}` | `<minisPersistentBase>/<sid>/<bucket>` | path-translate hook（`MinisFsRouter`） |
| `/var/minis/{memory,skills,shared}`、`/var/minis/mcp-servers` | `<AppGroup>/MinisFileProvider/...`、`<AppGroup>/MinisConfig/mcp-servers` | `fakefs_bind_mount`（`ISHExecutionCoordinator.performMount`） |
| `/var/minis/mounts/<name>` | 書籤解析出的使用者資料夾 | `bindMountPath`（`MacMounts`） |
| 網路盤 | Rclone.xcframework 的 Go netfs | `netfs_set_host_ops` |
| `/etc/resolv.conf` | `<Library>/MinisChat/dns/resolv.conf` | 檔案級 bind mount |

VM 相關的一個細節：guest 對**唯讀開啟的檔案做 `MAP_SHARED` 唯讀映射**時，Hypervisor.framework 拒絕直接映射這種宿主記憶體，iSH 改給 VM 一份內容快照。之後別的行程改寫那個檔案，這個映射看不到（JIT 模式看得到）。實務上少見（node 讀 ICU 資料檔就是這種，無害）。

### 3f. 環境變數與注入（VM 模式下 iSH 自己做的事，App 不要重複）

| 項目 | JIT 模式 | VM 模式 |
|---|---|---|
| node V8 旗標 | `--no-sparkplug --no-maglev --no-opt --regexp-interpret-all ...`（或 jitless 組） | **只有 `--no-sparkplug`** |
| node `LD_PRELOAD=/lib/zero_free.so` | 注入 | **不注入** |
| `PYTHONMALLOC=malloc` | 注入 | **不注入**（用 pymalloc） |
| `GOMAXPROCS=2` | 注入 | **不注入** |
| `UV_THREADPOOL_SIZE=1` | 注入 | **不注入** |
| `GODEBUG=asyncpreemptoff=1` | 注入 | **保留**：Go 的非同步搶佔在 iSH 的訊號實作下會壞，JIT 模式也是（esbuild 12 次壞 2 次，§7 [G1]） |
| `NO_COLOR=1`、`PIP_PROGRESS_BAR=off`、`PYTHONDONTWRITEBYTECODE=1` | 注入 | 保留 |

這些規則在 `kernel/exec.c`（每次 execve）和 `xX_main_Xx.h`（CLI 第一個行程）裡；App 的注入要照 3b 調整。

### 3g. 網路

透明轉發（xnet）是 iSH 核心 socket 層，VM 不影響：接線點照舊是 `ISHKernel (TransparentNet)`（`xnetConfigureEntryPort`、`xnetSetRoutesGeneration`→`ish_xnet_set_routes2`、`xnetSetEnabled`），由 `ShellTransparentNet.swift` 驅動。代理環境變數（`ALL_PROXY` 等）只在 agent 命令路徑加，終端沒有——現狀，與本次接入無關。

子代理調查發現（未驗證）：Mac 上找不到開機時套用已存網路設定的呼叫（iOS 在 `AppDelegate.swift:83` 呼叫 `NetworkHub.shared.bootstrap()`，該檔被 Mac target 排除）。

---

## 4. 簽名、entitlement 與發行

### 4a. 要加的授權

VM 需要 **`com.apple.security.hypervisor`**（布林 true）。三套 entitlements 目前**都沒有**：

| 檔案 | 用於 | 建議 | 依據 |
|---|---|---|---|
| `src/macos/Resources/MinisMac.entitlements` | Debug、Release（Developer ID） | **加** | 本機 ad hoc＋hardened runtime 已驗證能建 VM；Developer ID 簽名與公證**未驗證** |
| `MinisMac-Local.entitlements` | 命令列 ad hoc | **加** | 已驗證（同上） |
| `MinisMac-AppStore.entitlements` | Release-AppStore（沙盒） | 技術上可加 | 本機實測：**沙盒 App 套件**（ad hoc、hardened runtime、`app-sandbox`＋`hypervisor`＋`allow-jit`）`hv_vm_create` 成功；同一套件不加 `hypervisor` 則 `HV_DENIED (0xfae94007)`（§7 [E1]）。**Mac App Store 審核是否接受此授權：未驗證** |

加法（plist）：
```xml
<key>com.apple.security.hypervisor</key>
<true/>
```

### 4b. 降級、allow-jit 與當機處理器

- 沒有授權或沒有 Hypervisor.framework：**靜默退回 JIT**。JIT 仍需要 `com.apple.security.cs.allow-jit`（三套都已有），兩個授權在 hardened runtime 下可並存（已驗證）。
- **必做：當機處理器要先轉給 fastmem**。新 libish 在 JIT 模式下預設開 fastmem（native code 經一個按頁保護的窗口別名存取 guest 記憶體，碰到不能存取的頁由 SIGSEGV/SIGBUS 處理器接手）。iSH CLI 的處理器第一步就是 `jit_fastmem_fault(ctx)`；**App 的 `jit_crash_handler`（`ISHKernel.m:495`）沒有**。不加的話，一旦退回 JIT（沒授權、`ISH_HVF=0`、執行緒超過 64 個 vCPU），第一次碰到保護頁就會被當成崩潰處理。在 `pthread_main_np()` 檢查之後加上：
  ```objc
  // 檔案前段，和既有的 extern bool jit_crash_sync(...) 放在一起
  #if TARGET_OS_OSX
  extern bool jit_fastmem_fault(void *ucontext);
  #endif

  // jit_crash_handler() 裡，pthread_main_np() 那段之後
  #if TARGET_OS_OSX
      if ((sig == SIGSEGV || sig == SIGBUS) && jit_fastmem_fault(ctx))
          return;
  #endif
  ```
  函式在 libish 的 `asbestos/guest-arm64/jit.c`，宣告在 `asbestos/guest-arm64/jit.h`，但 `build_ish_macos.sh` 的 `copy_headers` 不複製 `asbestos/guest-arm64/*.h`，所以像 `jit_crash_sync` 一樣在 ISHKernel.m 裡自己宣告。VM 模式下 fastmem 本來就關，這個呼叫立刻回 false。不想改處理器的話，次選是開機前 `setenv("ISH_FASTMEM", "0", 1)`（JIT 退回時慢一些）。

### 4c. 開發時怎麼簽、怎麼確認 VM 真的生效

- `tools/hvf-sign.sh` 是 ad hoc，只給 CLI 用（meson 建 CLI 時會自動簽上）。App 照 Xcode 的簽名流程，授權寫進 entitlements 即可。
- **確認引擎**（任選）：
  - C／Swift：`hvf_describe(buf, size)`（`kernel/hvf.h`，只在 macOS）。第一行是 `engine: vm (...)` 或 `engine: jit (原因)`；開機前呼叫也行（會在那時決定）。可以把它印到 debug 日誌，或做進「關於／診斷」頁。橋接標頭加 `#include "kernel/hvf.h"`。
  - guest 內：`cat /proc/ish/hvf`。
  - 範例輸出（VM）：
    ```
    engine: vm (Hypervisor.framework), 40-bit guest physical addresses, 16-bit ASIDs
    vcpus: 1 in use, 2 made, 0 refused (those threads run in the JIT)
    address spaces: 1 in use, 4 made, 0 refused (those run in the JIT)
    exits: 870 runs, 866 syscalls, 435 faults (2 to the kernel), 2 kicks, 0 stage-2
    tlb: 21 flushes on entry, 242 synchronous
    memory: 16 2MB window blocks mapped, 6 data objects (0 copied)
    ```

### 4d. Developer ID、公證、沙盒

- Developer ID 簽名＋公證帶 `com.apple.security.hypervisor`：**未驗證**（本機只做了 ad hoc）。它不是需要描述檔的受限授權（推論）。
- 沙盒：見 4a。沙盒不影響 VM 本身；沙盒對檔案路徑的限制照舊。

### 4e. 失敗時的診斷

| 情況 | `hvf_describe` 第一行／計數 | stderr |
|---|---|---|
| 缺授權 | `engine: jit (no VM: the com.apple.security.hypervisor entitlement is missing)` | 只有設 `ISH_HVF=1` 時印 `iSH: no Hypervisor.framework VM (0xfae94007) ...` |
| 建 VM 失敗（其他原因） | `engine: jit (no VM: hv_vm_create 0x...)` | 同上 |
| `ISH_HVF=0` | `engine: jit (ISH_HVF=0)` | 無 |
| vCPU 用完（>64 條執行緒） | `vcpus: ... N refused` | 第一次印 `iSH: no vCPU for a guest thread (0xfae94005, 64 in use): it runs in the JIT` |
| 位址空間 ID 用完（>65,535 個行程） | `address spaces: ... N refused` | 第一次印 `iSH: no address space ID left in the VM ...` |
| 頁表區用完（256 MB，約 65,536 張頁表） | 無專門計數 | 該行程的缺頁會一直交給核心，最後以 SIGSEGV 結束（推論：理論極限，實測未碰到） |

---

## 5. 驗收清單

### 5.1 可直接跑的命令（在 Minis 終端或 agent 裡）

先 `cat /proc/ish/hvf`，第一行要是 `engine: vm`。以下倍數來自 realfs 基準（§7 [T1]）；fakefs 的差距見 3d 的表。

| 命令 | 期望輸出 | VM 耗時量級（docker 倍數） |
|---|---|---|
| `python3 -c 'print(sum(i*i for i in range(3000000)))'` | `8999995500000500000` | 約 0.2 s |
| `for i in 1 2 3 4 5; do python3 -c pass; done` | 無 | 約 0.06 s（2.0×） |
| `python3 -c 'import multiprocessing as m; print(m.Pool(2).map(abs,[-1,-2]))'` | `[1, 2]`（要先做 3d 的 `/dev/shm`） | — |
| `node -e 'let s=0; for (let i=0;i<3e8;i++) s+=i%7; console.log(s)'` | `899999997` | 約 0.45 s（docker 含容器啟動 0.74 s） |
| `node -p process.execArgv.join(" ")` | 含 `--no-sparkplug`，沒有 `--jitless`/`--no-opt` | — |
| `git clone -q <本地 bare repo> /tmp/x && git -C /tmp/x log --oneline \| wc -l` | 提交數 | 2.6–2.8× |
| `gcc -O1 -c sqlite3.c`（sqlite amalgamation） | 成功 | 1.6× |
| `pip install --no-index --find-links <wheels> requests` 進 venv | 安裝成功 | 1.8× |
| `npm ci --offline`（有快取） | 成功 | 1.8× |

### 5.2 相容套件與回歸

在 iSH 倉庫（`deps/ish-macos`）裡，用 CLI（`deps/libs/macosx/ish`，或 meson 建的 `build/ish`，已自動簽授權）：

```bash
# 回歸（需要 aarch64-linux-musl-gcc）
tests/regress/run.sh -i <ish> -r <realfs rootfs>              # 預設 = VM
ISH_HVF=0 tests/regress/run.sh -i <ish> -r <realfs rootfs>    # JIT
# 相容套件（240 項，fakefs rootfs）
benchmark/aojit/compat.sh <ish> <fakefs rootfs>
```

期望：回歸兩種模式 `regress: all cases passed`；相容套件 **240/240**（我在同負載下逐項 JIT/VM 背靠背跑，兩邊都是 240/240，§7 [C1]）。會因環境逾時的：`npx -y` 類 Skill 用例（60 s 上限，需要網路與 npx 快取）；主機很忙時（load ≥ 10）任何 15 s 上限的用例都可能逾時，重跑即可。

### 5.3 已知差異與坑

- `ISH_HVF=0`：回到 JIT（一鍵退回）。
- node 在 VM 下自動補 `--no-sparkplug`（3d）。
- flask 壓測（每請求一條新連線、5,000 次×4 輪）會把 macOS 的臨時連接埠用完（`EADDRNOTAVAIL`，TIME_WAIT 30 s、埠範圍 49152–65535）：VM 和 JIT 都碰過，是 iSH 直接用宿主 socket 的既有限制。
- pytest 在 **realfs** 下需要 `--basetemp=/tmp/...`（宿主 uid 與 guest 不同）；App 用 fakefs，不受影響。
- `/dev/shm`：見 3d，App 要自己建。
- 「寫入夾在兩個映射之間的空洞」不會 SIGSEGV：iSH 既有語義（JIT、VM 都是），讀 PROT_NONE 頁也不會報錯。
- Go 程式保持 `GODEBUG=asyncpreemptoff=1`（3f）。

### 5.4 回滾

- 執行期：開機前 `setenv("ISH_HVF", "0", 1)`，或不帶 `com.apple.security.hypervisor` 授權 → JIT。
- 版本：把 `deps/ish-macos` 指針退回上一個 commit 並重建。

---

## 6. 風險與未驗證項

| 項目 | 狀態 | 說明 |
|---|---|---|
| iOS 零變化 | **已驗證** | 集成分支 vs iOS 現在的樹（stage2 ⊕ deps/ish），iOS 參數建置目標檔 94/94 一致；`kernel/hvf.c` 在 iOS 上編出空目標檔 |
| 回歸套件（VM、JIT） | **已驗證** | 全過，含 xnet TCP 113、UDP/DNS 72、長命令列 7、hook 路徑 6 |
| 相容套件 240 | **已驗證** | JIT 240/240、VM 240/240（`explore/mac-hvf` 43956e9a；集成分支 2cd8d853 同樣 JIT 240/240、VM 240/240） |
| 沙盒下 hypervisor | **已驗證（本機 ad hoc）** | 沙盒 App 套件能建 VM；App Store 審核**未驗證** |
| Developer ID＋公證 | **未驗證** | |
| fakefs 下性能 | **已驗證（11 個場景）** | 見 3d |
| 超過 64 條執行緒（混合 VM/JIT） | **部分驗證** | Python 100 執行緒、jest 52 條退回 JIT、node --test 正確；V8 程式碼被 VM 執行緒改寫後 JIT 執行緒跑到舊翻譯的可能性**未排除** |
| 多終端並發 | **未驗證** | App 端「一次一個 shell」限制與 VM 無關，per-tab pty 改造未做 |
| xnet stage 2 | **已驗證** | 回歸 UDP/DNS 72 項（VM、JIT） |
| 長時間穩定性 | **已驗證（40 分鐘）** | 同一個 iSH 行程連跑 40 分鐘混合負載（python、pytest、node、tsc、esbuild、git clone、lua 的 make -j4、sort、8 個並行 python）：**190 輪全部正確**；每輪耗時不漂移（前 1/4 中位數 13 s、後 1/4 12 s）；每輪結束時 VM 裡的 vCPU 一直是 13 個；宿主 RSS 隨負載在 55–370 MB 起伏，頭尾峰值相同（363 MB／361 MB）。另外連續建 500 個行程：位址空間 1006 個建立後只剩 1 個在用，vCPU 503 個建立後剩 1 個，沒有 vCPU、ASID 洩漏。更長時間（數小時、數天）：未驗證 |
| Go 非同步搶佔 | **已知問題** | iSH 訊號實作與 Go 搶佔不相容（JIT 也是），保留 `GODEBUG=asyncpreemptoff=1` |
| 唯讀共享檔案映射 | **已知差異** | VM 看到快照（3e） |
| Intel Mac | **不支援 VM** | 只有 Apple silicon；Intel 上不會有 VM（照舊用現有引擎） |
| macOS 版本 | **推論** | 用到的 API 從 macOS 11 起有，`hv_vm_config_*` 要 13；App 最低 14.0，沒問題 |

---

## 7. 數據來源

結果目錄：`/Volumes/External HD/ish-work/macjit/results/20261027rw/`。報告：`docs/macos/realworld-report.md`（階段四，含 §3 完整耗時表）、`docs/macos/ledger-report.md`（階段二開銷帳本）。

- [T1] 真實場景總表：`05-final/`（docker、VM=43956e9a、JIT=同一執行檔 `ISH_HVF=0`，同時段交替，3 輪熱中位數）；jest、node_test 用 `05b-jest/`（2cd8d853，修正 vCPU 用完時的 fastmem 問題後重跑）；iOS＋AOT、iOS gadget 欄來自 `02-ios-compare/`（d511e83c，iOS Release 參數）。
- CoreMark：提交 1e565e47 的說明（同機 docker、JIT 對照）。
- [B1] MinisApp 參數建置：`/tmp/cm/macapp/`（native file、release、b_ndebug、nslog、jit_emit）。
- [V1]–[V4] 集成分支驗證：回歸 `/tmp/cm/regress_integ_{vm,jit}.log`（74bb5b05）、`/tmp/cm/regress_final_{vm,jit}.log`（2cd8d853）；iOS 比對 `/tmp/iosv2/`（`objs-ref` = iOS 樹，`b` = 2cd8d853，94/94）。
- [C1] 相容套件：`/tmp/cm/compat_hvf2.log`（JIT 240、VM 240）、`/tmp/cm/compat_integ.log`（集成分支 2cd8d853：JIT 240、VM 240）。
- [S1] `/dev/shm`、black：本次會話實測（VM，realfs）。
- [M2] vCPU 佔用：`/proc/ish/hvf`（2cd8d853）。[M3] jest：`05b-jest/`。
- [E1] 沙盒＋hypervisor：探針 `hvprobe`（`HVProbesb.app` 成功、`HVProbesbno.app` 0xfae94007）。
- [N1] node 22.15.1 Sparkplug：原生 docker 以 musl loader 執行 fakefs rootfs 裡的同一個 node 二進位，崩潰相同；加 `--no-sparkplug` 正常。
- [G1] Go：esbuild 在 JIT 下開搶佔 12 次壞 2 次；VM 下關搶佔 30/30。
- [F1] fakefs：`06-fakefs/`。
- 浸泡：`07-soak/`（`rounds.log` 每輪耗時與 vCPU，`rss.log` 每分鐘宿主 RSS），腳本 `/opt/macjit/soak.sh`（在 `alpine-arm64-rw` 裡）；行程數檢查是本次會話實測（`/proc/ish/hvf`）。
