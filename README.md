# PRL NPU Miner（Pearl · Snapdragon 8 Elite）

在骁龙 8 Elite（SM8750）的 Hexagon v79 NPU（cDSP，HVX + HMX）上挖 Pearl（PRL）的 Android 应用。
矿工作为普通 App 运行，通过 Hexagon FastRPC 直接调用 NPU，**不需要 root、Shizuku 或 adb**。

- 界面：Kotlin + Jetpack Compose（Material 3 蓝黑主题）
- 监控：算力曲线（TMAC/s，1 秒采样）、NPU/CPU 温度、电池温度与功率、内存、accepted/rejected 计数
- 权限：仅 `INTERNET`、`ACCESS_NETWORK_STATE`、`WAKE_LOCK`、`FOREGROUND_SERVICE`、`FOREGROUND_SERVICE_SPECIAL_USE`、`POST_NOTIFICATIONS`
- 矿池与钱包地址完全由用户输入，默认留空，不内置任何账号信息

## 安装

1. 从 [Releases](../../releases) 下载对应版本的 APK
2. 侧载安装（允许“未知来源”）
3. 启动后等待自检完成，进入仪表盘

## 使用

1. 输入矿池地址（stratum，如 `pool.example.net:3333`）、PRL 钱包地址（`prl1p...`；Kryptex 协议下也可填矿池用户名/邮箱）、worker 名
2. PPS+/PPLNS 只是本地记忆的偏好，PPLNS 时 worker 名追加 `.pplns`；实际结算方式以矿池网页端设置为准
3. 在“开发者费用”中选择关闭、1% 或 5%；启用后每次切换 1 分钟，分别约每 100 或 20 分钟一次，费用窗口份额不计入你的统计
4. 点击“开始挖矿”，阅读风险提示，勾选确认并等待 5 秒后启动
5. 仪表盘实时显示算力、温度与份额计数；通知栏常驻前台服务显示当前算力

## 版本与分支

每个矿工版本是一个分支，`main` 是目前表现最好的版本。APK 会随每个版本的 release 发布。

| 分支 | 内核 | 实测吞吐（真实矿池，1 秒窗口） | 说明 |
|---|---|---|---|
| `main` | early byte-fold + deep1 / PF0 / G16 | 6.990 / 6.916 TMAC/s（两轮分开统计） | 当前默认：对同组旧 fold 提升 +8.58% / +10.49%，共 158 个完整窗口 |
| `8gen3/adapt` | 同 main，Hexagon **v75** 构建；双 SoC APK | SM8650 目录内核：真池长测 ~4.17 TMAC/s、20s bench 4.2 TMAC/s（E0 编排，见验证记录 §9） | 面向 Snapdragon 8 Gen 3（SM8650）的架构适配：一个 APK 携带全部受支持 SoC 目录（`SM8750/`、`SM8650/`）的最优载荷，自检按 SoC 选择并预先装载 |
| `v28` | row-first + L2FETCH，G16 | 7.1–7.4 TMAC/s（历史短窗） | 正式基线版本，长期真机验证最充分 |
| `v27` | 因子预取 | ≈6.04 TMAC/s | 历史版本 |
| `v26` | 跨 slice 预取 | ≈5.68 TMAC/s | 历史版本 |
| `v25` | 紧凑输入 + L2FETCH | ≈4.05 TMAC/s | 历史版本 |
| `v24` | 紧凑输入 | ≈2.01 TMAC/s | 历史版本 |

性能数字均为特定真机上的短窗口实测，受温度、时钟驻留和热漂移影响，不代表持续稳态，也不构成对不同设备的承诺。
当前 main 的数据来自相反顺序 ABBA / BAAB 两组真实池对照，分别为 6.438→6.990、6.259→6.916 TMAC/s；固定 4 个 ARM prep workers，每腿 30 秒，未充电并持续简单风冷。两组都有热降频，不与历史冷机短窗横比或累乘增益。完整 CPU 对拍和实际 adapter 检查通过，share 提交仍未验证。见[验证记录](docs/validation/bytefold-early-20261113/README.md)。

## 硬件与系统要求

- SoC：Snapdragon 8 Elite（SM8750，Hexagon v79 cDSP）；`8gen3/adapt` 分支另支持 Snapdragon 8 Gen 3（SM8650，Hexagon v75 cDSP，`PRL_SOC=SM8650` 构建 DSP 载荷）
- 系统：Android 12（API 31）及以上，arm64
- 设备需带 `libcdsprpc.so` 与可访问的 FastRPC 设备节点（`/dev/fastrpc-cdsp`，或部分 8 Gen 3 机型仅有的多路复用节点 `/dev/adsprpc-smd`）

App 每次启动都会自检：SoC 厂商/型号、FastRPC 运行库、APK 载荷完整性，并跑一次 **CPU + Hexagon HMX 对官方 golden 向量的逐位自检**；
随后对实际 compound 内核跑一次无网络的 app-check（2 jobs / 4 batches / 128 nonces）。
任何一项失败都会显示“此机型不支持”并禁用挖矿，不会在错误硬件上计算。

## ⚠️ 安全与风险须知

- **没有自动过热保护**。HMX 满载时 NPU 传感器可在数秒内超过 95 °C。请自行监控面板温度并保证散热（外置风冷、不充电、去掉保护壳）。风险由使用者自行承担。
- 持续满载会加速电池老化，并可能触发系统降频或保护性关机。
- 性能数字为工程实测短窗，未做长时间热稳态验证；不同机型、不同散热条件结果会不同。
- 挖矿收益取决于矿池与网络，本项目不对收益作任何承诺。

## 技术构成

```
app/             Android 应用（Kotlin + Compose）
miner/           矿工主体（C，stratum + Pearl proof + FastRPC 适配），含 golden 向量与 mock 矿池测试
dsp/fused/       融合单次路径 DSP skeleton（自检/回退路径）
dsp/compound/    复合批处理 DSP skeleton（生产路径，版本差异所在）
SM8750/          Snapdragon 8 Elite 的 DSP 载荷（构建产物 + 说明）
SM8650/          Snapdragon 8 Gen 3 的 DSP 载荷（构建产物 + 说明）
common/          架构无关的矿工与 app-check（构建产物 + 说明）
scripts/         构建脚本（取依赖、DSP、矿工、APK）
.github/         CI：从源码构建全部载荷并产出/发布 APK
```

App 内置**每个受支持 SoC 一套** DSP skeleton（`lib*_skel_SM8750.so`、`lib*_skel_SM8650.so`）、一份 `libprlminer.so` 和一个 app-check 检查器，全部由 CI 从源码编译，仓库不含任何二进制产物。
自检阶段按 SoC 解析载荷组并把对应一套 skeleton 复制成 FastRPC 规范名（`DspArch.kt` 映射表，`MinerRunner.prepare()` 落盘），`ADSP_LIBRARY_PATH` 只指向选中目录；点击开始挖矿后无需再加载。
miner 每秒输出一次统计行；compound RPC 带 fail-fast 保护，任何 RPC 错误都会停止挖矿而不是提交伪结果。

## 从源码构建

需要：JDK 17、Gradle 8.5+、Android SDK（platform 36 / build-tools 35.0.0）、Android NDK r27c、
Hexagon SDK 6.3.0（含 Hexagon Tools 19.0.04）、HexKL 1.0-beta.2 addon。

外部工具链都不放在本仓库：

- Hexagon SDK 6.3.0：CI 用社区镜像 [snapdragon-toolchain/hexagon-sdk](https://github.com/snapdragon-toolchain/hexagon-sdk/releases/tag/v6.3.0)（官方分发走 [Qualcomm Package Manager](https://www.qualcomm.com/developer/software/hexagon-npu-sdk)）
- Android NDK r27c：[dl.google.com/android/repository/android-ndk-r27c-linux.zip](https://dl.google.com/android/repository/android-ndk-r27c-linux.zip)
- HexKL 1.0-beta.2：[Qualcomm 软件中心直链](https://softwarecenter.qualcomm.com/api/download/software/tools/Hexagon_KL/Linux/Debian/1.0.0-beta2/hexkl-1.0-beta.2.zip)（`fetch-deps.sh` 已内置；可用 `HEXKL_URL`/`HEXKL_ROOT` 覆盖）

```bash
# 1) 拉取工具链到 deps/
bash scripts/fetch-deps.sh

# 2) 构建每个受支持 SoC 的 DSP 载荷与架构无关的矿工
export HEXAGON_SDK_ROOT=$PWD/deps/hexagon-sdk/6.3.0.0
export HEXKL_ROOT=$PWD/deps/hexkl/hexkl_addon
export ANDROID_NDK_ROOT=$PWD/deps/android-ndk-r27c
bash scripts/build-dsp.sh                 # SM8750（默认）-> SM8750/
PRL_SOC=SM8650 bash scripts/build-dsp.sh  # SM8650        -> SM8650/
bash scripts/build-miner.sh               # ARM 侧        -> common/

# 3) 组装 APK（自动 pickup 全部已构建 SoC 目录，debug 或 release）
bash scripts/build-apk.sh debug
```

新增受支持 SoC（如 8 Gen 5 类机型）：在 `build-dsp.sh` 的 `SOC_TABLE` 与
`build-apk.sh` 的 `PAIRS` 各加一行、`DspArch.TARGETS` 加一个条目，构建出
`SMxxxx/` 目录即可自动进 APK。

已有本地工具链时，直接设置对应环境变量即可，无需 `fetch-deps.sh`。

### GitHub Actions

- **CI**（每次 push / PR）：只跑廉价检查——Kotlin 编译 + 脚本语法，不做任何下载/打包。
- **Release APK**（手动触发）：Actions → Release APK → Run workflow，选择分支并输入新 tag（如 `v1.0.0`），批准环境审批后自动从源码构建全部载荷、打 tag、签名并发布 Release。
CI 开箱即用；如需改用自备镜像，可在 Settings → Secrets and variables → Actions 里配置 `HEXKL_URL`（私有镜像请建为 **Secret** 而非 Variable）。

**签名密钥（可选）**：在 Settings → Environments 新建名为 `release` 的 environment，把 `APK_KEYSTORE_B64` / `APK_KEYSTORE_PASSWORD` / `APK_KEY_ALIAS` / `APK_KEY_PASSWORD` 四个 Secret 建在其中，并给该 environment 开启 Required reviewers（你自己）。这样签名密钥只存在 GitHub 服务器、repo 与 workflow 文件里永不出现，且任何 workflow 运行要读它都必须先经你人工批准；未配置时发布 debug 签名 APK。

说明：Hexagon 链接器输出非字节级确定（PLT 顺序随机），不同机器/不同次构建的 .so 哈希不同属正常现象；每个 Release 附带本次构建的 SHA256SUMS。

### 应用标识与图标

应用包名为 `com.tridefender.npupearl`。更换包名后 Android 会将其视为独立应用，旧包的设置不会自动迁移。
启动图标采用手绘 PRL 字母组合，支持自适应裁切与 Android 13+ 单色主题图标；源图和设计说明见 `app/design/ICON.md`。

### 设备上手动验证

```bash
adb install -r out/prl-npu-miner-*.apk
adb shell am start -n com.tridefender.npupearl/.MainActivity
```

`miner/tests/` 内含官方 golden 向量与 mock 矿池（`mockpool.py`），可在推送前做离线校验。
`common/app-check` 是实际矿工 adapter 检查器（M=2048,N=64,K=2048）；`common/bytefold-batch32-driver` 覆盖生产尺寸 M=2048,N=1024,K=2048 的 32 nonce 全量 commitment/raw TT/digest/mask CPU 对拍。手动运行时设置 `PRL_EXPECT_BYTEFOLD_EARLY=1`，以载荷目录（SoC 目录在前）为 CWD 和分号分隔的 `ADSP_LIBRARY_PATH` 首项；App 已设置相同约束。

## 已知限制

- 仅支持 8 Elite（SM8750/v79）与 8 Gen 3（SM8650/v75，`8gen3/adapt` 分支）；其他 Hexagon 版本（v68/v69/v73/v81）未适配。架构选择发生在自检：8 Gen 3 只能加载 v75 载荷（v79 被固件拒载）；v79 机型优先用原生 v79 载荷（v75 载荷因前向兼容也能加载，但只有 2 workers，性能打折）
- share 提交格式尚未在矿池端命中验证（实测轮次 accepted/rejected 均为 0/0，只是没有命中，不代表格式正确或错误）
- Android 16 的普通应用读不到 `/proc/stat`，CPU 占用为系统负载/频率估算值
- 功率显示为电池整机功率，不是 NPU 单独功耗
- debug 与 CI 签名的 APK 不适用于应用商店分发

## 许可

矿工与应用代码按仓库现状提供，不附带任何担保。BLAKE3 实现为上游 CC0-1.0（见 `miner/blake3/`）。
Hexagon SDK、HexKL、Android 等外部组件归各自权利人所有，按其许可使用。
