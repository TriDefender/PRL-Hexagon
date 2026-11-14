# Snapdragon 8 Gen 3（SM8650）适配记录 — 2026-11-14

目标设备：OnePlus PJD110（一加 Ace 3 Pro），SM8650（Snapdragon 8 Gen 3），
Android 16，kernel 6.1.141-android14，arm64，SELinux enforcing。
测试钱包：`prl1p9z3057569n4k6lp7tfalupalkfj3njuek5r2fk79653q80z273as266sf6`，
矿池：`pool.peryn.ai:3333`。

## 1. 结论概要

- main 的构建**不能**在 8 Gen 3 上工作，两个独立原因：
  1. DSP 载荷按 `-mv79` 编译；SM8650 的 cDSP 是 **Hexagon v75**，加载 v79 ELF
     返回 `0x80000401 (AEE_EUNSUPPORTED)`（真机实测）。
  2. App 自检只放行 `SM8750` / board `sun`。
- 8 Gen 3 对应的 Hexagon 微架构是 **v75**（cDSP 原生目标；另有 v73 兼容目标）。
- 对应手册（对照 main 使用的 v79 文档）：
  - 核心 PRM：**80-N2040-57 Rev.AB**（Qualcomm Hexagon V75 PRM）↔ main 用的 80-N2040-60（V79）
  - HVX PRM：**80-N2040-58 Rev.AB**（Qualcomm Hexagon V75 HVX PRM）↔ main 用的 80-N2040-61（V79 HVX）
  - HMX：main 实际依据的 HMX 指令文档是 **80-N2040-62 Rev.AA**（Qualcomm Hexagon V81 HMX PRM，
    2026-03 初版），其中定义了 `mxmem`、activation/weight deep 等指令；v75/v79 均由汇编器
    按 `-mv7x` 目标分别校验。
- 整套内核（deep4 HMX packet、early byte-fold HVX、HexKL micro u8i8）**源码级无需修改**，
  用 `-mv75` + `computev75` QURT + HexKL `hexagon_toolv88_v75` 重编译即通过——
  汇编器接受了全部指令（含 `mxmem(...):deep:cm`），HexKL 1.0-beta.2 官方附带 v75 库。
- 尚需真机确认的行为差异：deep4 packet 的 RT 编码语义、VTCM 实际容量（布局目标 ~8 MiB）、
  HMX 时钟/吞吐。由 app-check golden 对拍 + selftest 逐位校验兜底。

## 2. 设备侧事实（实测）

- `/dev/fastrpc-cdsp` **不存在**（OnePlus/ColorOS 的 SM8650 内核只保留多路复用的
  `/dev/adsprpc-smd` + `adsprpc-smd-secure`）；vendor `libcdsprpc.so` 存在并会回退打开
  `/dev/adsprpc-smd` 建立 CDSP 会话（实测 app-check 的 fd 打开的是该节点）。
- 用 main 的 v79 载荷跑 `app-check`：`compound DSP RPC failed rc=0x80000401` →
  FastRPC 通道正常、DSP 拒载 v79 ELF。
- msm_fastrpc compute banks cb1..cb17 已注册，DSP 固件运行正常
  （`/vendor/lib/rfsa/adsp/` 有 16 个系统 skeleton）。

## 3. 分支改动（8gen3/adapt）

- `scripts/build-dsp.sh`：新增 `PRL_DSP_ARCH`（默认 `v79` 保持 main 行为；
  `v75` 时用 computev75 QURT 头 + HexKL v75 库），并前置校验目录存在。
- `SelfCheck.kt`：平台门控同时接受 `SM8650` / `pineapple`（8 Gen 3）；
  fastrpc 节点检测接受 `/dev/fastrpc-cdsp`、`/dev/cdsprpc-smd`、`/dev/adsprpc-smd` 任一。
- 其余（dsp/miner）零修改：v75 与 v79 的 ISA 差异没有触及本内核用到的指令子集。

## 4. 构建

```bash
export HEXAGON_SDK_ROOT=…/hexagon-sdk-6.3.0.0
export HEXKL_ROOT=…/hexkl_addon            # 1.0-beta.2，lib/6.3.0.0 含 hexagon_toolv88_v75
export ANDROID_NDK_ROOT=…/android-ndk-r27c
PRL_SOC=SM8650 bash scripts/build-dsp.sh    # -> SM8650/；一次通过，无 ISA 错误
bash scripts/build-miner.sh                 # -> common/
```

（初版接口为 `PRL_DSP_ARCH=v75` 输出 `payloads-v75/`，后随 SoC 目录重组改为
`PRL_SOC`，见 §6。）

ELF 校验：`hexagon-readelf -h` → Flags: 0x75, V75（对照 v79 载荷 0x79, V79）。
`check-dsp-elf.py`：PASS（无未解析 hexkl_* 导入）。

## 5. 运行时验证（真机，全部通过）

日期 2026-11-14，载荷目录 `/data/local/tmp/prl-v75`：

- `app-check`（adapter + golden 对拍）：**APP_CHECK_PASSED**（jobs=2 batches=4 attempts=128，
  scalar 全量 attempts=2），`runtime_deep4_mode=1 runtime_prefetch_order=0
  runtime_bytefold_early=1 runtime_bytefold_tag=0x42464501`。
- `libprlminer --selftest golden.txt`：**PASSED**（`qnpu tile(0,0) digest == golden (NPU GEMM exact)`，
  share bincode+base64 == official）。
- `libprlminer --npu --bench 20`：**3089.36 GMAC/s**（14400 attempts / 20s；热身段 2574→3500 GMAC/s 波动，
  未做长时热稳态）。对照 8 Elite main ≈7 TMAC/s（v79、4 workers、更高时钟）。
- **真池测试**：`pool.peryn.ai:3333`，wallet `prl1p9z3…266sf6`，worker `8gen3-adapt`，
  diff=2097152，job 随高度正常切换（height 142892→142893），持续 ~3000 GMAC/s，
  期间 accepted/rejected=0/0（未命中 share，与 main 在 8 Elite 的验证状态一致，不代表格式错误）。

### 真机调试过程记录（重要结论）

1. v79 载荷在 SM8650 上：FastRPC 会话成功（vendor `libcdsprpc.so` 回退打开
   `/dev/adsprpc-smd` 多路复用节点），DSP 拒载 v79 ELF → `AEE_EFAILED (0x80000401)`。
2. v75 重编译后 fused skeleton 正常，compound skeleton 仍拒载：compound 的 `impl.c`
   弱引用 `HAP_power_get`（HMX 时钟查询，v79 时代固件 API），SM8650 固件不导出该符号，
   DSP 加载器对含此 UND 导入的 ELF（即使 weak）拒绝加载。移除导入（v7x 用本地
   unsupported stub，diag 记录标记）后加载成功；ARM 侧 summary 校验同步接受
   `rc=HAP_POWER_ERR_UNSUPPORTED_API, Hz=0` 的时钟槽位。`__hexagon_udivdi3`
   v75 固件有导出，无需处理。
3. compound 生产形状（n=1024）加载成功但 invoke 失败：热路径按 `PRL_GROUP_WORKERS=4`
   编译期特化（`worker[(cb>>1)&3]` 硬编码 4 路由），且 producer + async stage 线程 +
   4 workers = 6 个并发 `qurt_hvx_lock(128B)` 用户；v79 cDSP 有 8 个 HVX context，
   **v75 cDSP 只有 4 个**（QURT computev75 头：`QURT_HVX_HW_UNITS_4X128B_0X64B
   "V66 CDSP or newer"`），第 5 个锁失败 → worker status=AEE_EFAILED / 等待超时。
   v75 载荷改为**编译期 2 workers**（`scripts/build-dsp.sh` 的 v75 分支做 define 替换）
   + 运行时按 `qurt_hvx_get_units()` 自适应上限（kernel_impl.c），2+2=4 恰好用满预算。
   运行时减少 worker 而不改编译期特化是不可行的（热路径硬编码 4 路由）。

## 6. 双架构 APK（现按 SoC 目录组织）

一个 APK 携带全部受支持 SoC 的载荷，载荷按 SoC 命名的仓库根目录组织：
- 布局：`SM8750/`（v79）+ `SM8650/`（v75，`PRL_SOC=SM8650` 构建）各存该 SoC
  最优内核；`common/` 存架构无关 miner/appcheck（`build-miner.sh`）。
  `build-apk.sh` 按 SoC→e_flags 表自动 pickup 存在的目录，复制为
  `lib*_skel_SM8750.so` / `lib*_skel_SM8650.so` 进 jniLibs。后缀用 SoC 名而非
  架构名——两个 SoC 共享同一 Hexagon 目标时仍可携带各自调校的内核。
  `check-dsp-elf.py --arch 0xNN` 在打包时校验每个 skeleton 的 e_flags，
  防止架构错标。
- 选择与装载：`DspArch.kt`（SoC→目标映射表，新增 SoC = 表加一行）在自检
  "手机芯片/手机型号"两步解析目标；`MinerRunner.prepare()` 把对应一套 skeleton
  复制成规范名到 `filesDir/dsp/`，`ADSP_LIBRARY_PATH='<dspDir>;<nativeDir>'`，
  只有选中 SoC 的载荷可被找到。装载在自检 payload 步完成，点击开始挖矿后无额外加载。
- CI：release.yml 按 SoC 分别构建再打包；发布说明与 SHA256SUMS 覆盖各目录。

### 兼容性实测（2026-11-14，8 Elite PLQ110）

- 双目录搜索 + 规范名 staging：`ADSP_LIBRARY_PATH='<dsp>;<native>'` 下
  v79 载荷 APP_CHECK_PASSED。
- **v75 ELF 在 v79 cDSP 上同样加载成功且 golden 逐位一致**（Hexagon 前向兼容旧
  目标编译产物）；反向（v79 ELF 于 v75）被固件拒载。因此架构选择只影响
  "能否运行 + 是否最优"：8 Gen 3 必须 v75；8 Elite 优先原生 v79（4 workers），
  误装 v75 也能跑但只有 2 workers。
- 两台设备的 shell 模拟均通过；App 域（untrusted_app）SELinux 路径随后经 APK
  实际安装验证通过，见 §10。

## 7. 回归

- 8 Elite（PLQ110，SM8750/v79）：最终代码以 `PRL_DSP_ARCH=v79` 重建后
  `APP_CHECK_PASSED`——HVX 自适应 cap 在 v79 上取 8→budget 6→workers 保持 4，行为不变。

## 8. 兼容性结论与回滚矩阵（最终）

| 优化 | 依赖 | v75 状态 | 结论 |
|---|---|---|---|
| early byte-fold（mode 1） | HVX 基础指令 | 真机 golden 逐位一致 | ✅ 能跑 |
| deep4 HMX packet | `mxmem :deep:cm`，A_RT=6175/W_RT=3968 | 真机 golden 逐位一致 | ✅ 能跑（无需回滚到普通四对 packet） |
| HexKL micro u8i8 | 每架构官方库 | v75 库逐位一致 | ✅ 能跑 |
| G16 compound 布局 | ~8 MiB VTCM | init_rc=0，真机通过 | ✅ 能跑 |
| 4-worker fold 池 | 6 个并发 HVX context | v75 只有 4 个 | ❌ 不能跑 → v75 编译期 2 workers（实测 3.09 TMAC/s） |
| HAP_power_get 时钟查询 | v79 固件符号 | v75 固件无导出 | ❌ 不能跑 → v7x 时钟 stub（diag 标记 unsupported），挖矿不受影响 |

## 9. v75 编排优化 A/B（2026-11-14，真机 20s bench）

HVX context 预算 = 4（`qurt_hvx_get_units()` 实测 v75 cDSP 4×128B）。所有布局必须把
producer + async stage 线程 + fold workers 装进 4 个 context。

| 变体 | 布局 | GMAC/s（20s bench） | 结论 |
|---|---|---|---|
| E0（默认/最优） | producer + **stage 线程** + 2 workers | 4239→4201（冷→温） | **基准** |
| E1' | producer（内联 compact 噪声打包）+ **3 workers** | 3764（**-11%**） | ✗ 丢掉 stage 重叠代价大于 +1 worker |
| E2 | E0 + kernel 级 HMX 时钟投票 policy1（pick_default） | 4138（-2%） | ✗ 无增益 |
| E3 | E0 + kernel 级 HMX corner MAX（policy4） | 4177（-1%） | ✗ HMX 默认时钟已最高 |
| E4 | E0 + kernel 级 HMX corner TUR（policy3） | 4176（-1%） | ✗ 同上 |
| prep 亲和 {7,4,3,2} vs {7,6,5,4} | miner ARM prep pool 拓扑映射 | 4178 vs 4176（0%） | 预备完全被隐藏（RPC-bound），仅作拓扑保险 knob |

关键编排事实：
- **async stage 线程的重叠价值 > 第 3 个 fold worker**：stage 线程持有 HVX context
  做 `prl_noise_make_block`（compact 噪声打包，HVX 真活），把 ~15ms/批的 prep 从
  producer 关键路径完全隐藏；内联化后 producer 串行做打包，反而变慢。
- 8 Gen 3 CPU 拓扑：cpu0-1=A520 2.27G / cpu2-4=A720 3.15G / cpu5-6=A720 2.96G /
  cpu7=X4 3.3G。miner prep pool 默认 pins {7,6,5,4} 避开小核；新增
  `PRL_COMPOUND_PREP_CPUS="a,b,c,d"` 环境变量可按设备覆盖。
- DCVS 侧 E0 已通过 impl.c 的 policy1 投票锁 Q6/总线 MAX corner（所有 policy 都含
  DCVS_v3 MAX + sleep_disable）；kernel 级再加 HMX 投票只增加每 RPC 两个 power
  调用的开销。
- 最终生产编排（v75）：`PRL_GROUP_WORKERS=2` + `PRL_ASYNC_STAGE=1` +
  kernel `PRL_HMX_CLOCK_POLICY=0` + impl.c policy1 投票；真池长测持续
  **~4169 GMAC/s**（wallet prl1p9z3…266sf6 @ pool.peryn.ai:3333，worker 8gen3-e0）。
- 构建 knobs：`PRL_V75_WORKERS`（默认 2）、`PRL_V75_ASYNC_STAGE`（默认 1）、
  `PRL_V75_HMX_CLOCK`（默认沿用 build.conf）。

## 10. 双架构 APK 端到端验证（App 域，2026-11-14）

APK（HEAD `8faedaa` 构建，sha256 7717a61f…）内含 `_v75`/`_v79` 成对 skeleton +
架构无关 miner/app-check；DspArch.kt 在自检时解析 SM8650/pineapple → target=v75。

真机（PJD110，Android 16，ColorOS）安装后 **untrusted_app 域六项自检全部 PASS**：

```
soc        PASS 已检测到高通芯片
platform   PASS Snapdragon 8 Gen 3，支持 NPU 加速   (soc model=SM8650 board=pineapple target=v75)
hex        PASS NPU 驱动可用                        (fastrpc lib=true dev=true — /dev/adsprpc-smd 复用节点被接受)
payload    PASS 挖矿组件完整（v75 载荷已就绪）        (MinerRunner staging 到 files/dsp 规范名成功)
golden     PASS 计算校验通过，NPU 工作正常            (fused selftest 逐位通过，untrusted_app 域)
compound   PASS 复合内核校验通过，这台手机可以挖矿     (真实 compound 内核 2 jobs/4 batches/128 nonces)
```

FastRPC 侧确认：App 域成功创建 unsigned PD（domain 3）、从 `files/dsp/` 加载 v75
compound skeleton（load time ~5.9ms）、RPC 正常返回。

已知观察：安装后首次启动的 compound 自检曾挂起 >2 分钟（force-stop 后干净重启
复跑 1.8s 通过，且可稳定复现）。疑似与被杀进程遗留的 DSP 会话状态或首次冷启动
竞争有关；SelfCheck 的 1200ms 重试未能覆盖该 hang。建议后续给
`selftestCompound` 的子进程读取加超时强制（timeoutSec 参数已存在但未见强制逻辑）。

双架构互操作不再要求验证（单包单架构自检选择；跨架构编译/验证按设计不做）。
