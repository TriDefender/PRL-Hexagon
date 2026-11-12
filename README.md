# PRL NPU Miner（Pearl · Snapdragon 8 Elite）

在骁龙 8 Elite（SM8750）的 Hexagon v79 NPU（cDSP，HVX + HMX）上挖 Pearl（PRL）的 Android 应用。
矿工作为普通 App 运行，通过 Hexagon FastRPC 直接调用 NPU，**不需要 root、Shizuku 或 adb**。

- 界面：Kotlin + Jetpack Compose（Material 3 蓝黑主题）
- 监控：算力曲线（TMAC/s，1 秒采样）、NPU/CPU 温度、电池温度与功率、内存、accepted/rejected 计数
- 权限：仅 `INTERNET`、`ACCESS_NETWORK_STATE`、`WAKE_LOCK`、`FOREGROUND_SERVICE`、`FOREGROUND_SERVICE_SPECIAL_USE`、`POST_NOTIFICATIONS`
- 矿池与钱包地址完全由用户输入，默认留空，不内置任何账号信息

## 版本与分支

每个矿工版本是一个分支，`main` 是目前表现最好的版本。APK 会随每个版本的 release 发布。

| 分支 | 内核 | 实测吞吐（真实矿池，1 秒窗口） | 说明 |
|---|---|---|---|
| `main` | deep4 compound（deep1） | 7.081 TMAC/s（19 窗均值） | 当前最佳：同轮 A/B 比基线高 +1.61%，App 默认载荷 |
| `v28` | row-first + L2FETCH，G16 | 7.1–7.4 TMAC/s（历史短窗） | 正式基线版本，长期真机验证最充分 |
| `v27` | 因子预取 | ≈6.04 TMAC/s | 历史版本 |
| `v26` | 跨 slice 预取 | ≈5.68 TMAC/s | 历史版本 |
| `v25` | 紧凑输入 + L2FETCH | ≈4.05 TMAC/s | 历史版本 |
| `v24` | 紧凑输入 | ≈2.01 TMAC/s | 历史版本 |

性能数字均为特定真机上的短窗口实测，受温度、时钟驻留和热漂移影响，不代表持续稳态，也不构成对不同设备的承诺。
deep4 的 7.081 vs 6.969 TMAC/s 来自同一轮 ABBA 交错测量；v24–v28 的数字来自各自的 10×1 秒窗口测量，轮间条件不同，仅供了解版本间的相对进步。

## 硬件与系统要求

- SoC：Snapdragon 8 Elite（SM8750），Hexagon v79 cDSP（HVX + HMX）
- 系统：Android 12（API 31）及以上，arm64
- 设备需带 `libcdsprpc.so` 与 `/dev/fastrpc-cdsp`（8 Elite 机型默认都有）

App 每次启动都会自检：SoC 厂商/型号、FastRPC 运行库、APK 载荷完整性，并跑一次 **CPU + Hexagon HMX 对官方 golden 向量的逐位自检**。
任何一项失败都会显示“此机型不支持”并禁用挖矿，不会在错误硬件上计算。

## 安装

1. 从 [Releases](../../releases) 下载对应版本的 APK
2. 侧载安装（允许“未知来源”）
3. 启动后等待自检完成，进入仪表盘

## 使用

1. 输入矿池地址（stratum，如 `pool.example.net:3333`）、PRL 钱包地址（`prl1p...`）、worker 名
2. PPS+/PPLNS 只是本地记忆的偏好，PPLNS 时 worker 名追加 `.pplns`；实际结算方式以矿池网页端设置为准
3. 点击“开始挖矿”，阅读风险提示，勾选确认并等待 5 秒后启动
4. 仪表盘实时显示算力、温度与份额计数；通知栏常驻前台服务显示当前算力

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
scripts/         构建脚本（取依赖、DSP、矿工、APK）
.github/         CI：从源码构建全部载荷并产出/发布 APK
```

App 内置三个原生载荷（`libprlminer.so`、两个 DSP skeleton），全部由 CI 从源码编译，仓库不含任何二进制产物。
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

# 2) 构建 DSP skeleton 与矿工（默认输出到 payloads/）
export HEXAGON_SDK_ROOT=$PWD/deps/hexagon-sdk/6.3.0.0
export HEXKL_ROOT=$PWD/deps/hexkl/hexkl_addon
export ANDROID_NDK_ROOT=$PWD/deps/android-ndk-r27c
bash scripts/build-dsp.sh
bash scripts/build-miner.sh

# 3) 组装 APK（debug 或 release）
bash scripts/build-apk.sh debug
```

已有本地工具链时，直接设置对应环境变量即可，无需 `fetch-deps.sh`。

### GitHub Actions

- **CI**（每次 push / PR）：只跑廉价检查——Kotlin 编译 + 脚本语法，不做任何下载/打包。
- **Release APK**（手动触发）：Actions → Release APK → Run workflow，选择分支并输入新 tag（如 `v1.0.0`），批准环境审批后自动从源码构建全部载荷、打 tag、签名并发布 Release。
CI 开箱即用；如需改用自备镜像，可在 Settings → Secrets and variables → Actions 里配置 `HEXKL_URL`（私有镜像请建为 **Secret** 而非 Variable）。

**签名密钥（可选）**：在 Settings → Environments 新建名为 `release` 的 environment，把 `APK_KEYSTORE_B64` / `APK_KEYSTORE_PASSWORD` / `APK_KEY_ALIAS` / `APK_KEY_PASSWORD` 四个 Secret 建在其中，并给该 environment 开启 Required reviewers（你自己）。这样签名密钥只存在 GitHub 服务器、repo 与 workflow 文件里永不出现，且任何 workflow 运行要读它都必须先经你人工批准；未配置时发布 debug 签名 APK。

说明：Hexagon 链接器输出非字节级确定（PLT 顺序随机），不同机器/不同次构建的 .so 哈希不同属正常现象；每个 Release 附带本次构建的 SHA256SUMS。

### 设备上手动验证

```bash
adb install -r out/prl-npu-miner-*.apk
adb shell am start -n dev.prl.npuminer/.MainActivity
```

`miner/tests/` 内含官方 golden 向量与 mock 矿池（`mockpool.py`），可在推送前做离线校验。
`payloads/app-check` 是独立的设备端批量检查器，可在 adb 环境下对完整 nonce 批次做 CPU 对拍。

## 已知限制

- 仅支持上表列出的 SoC；其他 Hexagon 版本（v68/v73/v75/v81）未适配
- share 提交格式尚未在矿池端命中验证（实测轮次 accepted/rejected 均为 0/0，只是没有命中，不代表格式正确或错误）
- Android 16 的普通应用读不到 `/proc/stat`，CPU 占用为系统负载/频率估算值
- 功率显示为电池整机功率，不是 NPU 单独功耗
- debug 与 CI 签名的 APK 不适用于应用商店分发

## 许可

矿工与应用代码按仓库现状提供，不附带任何担保。BLAKE3 实现为上游 CC0-1.0（见 `miner/blake3/`）。
Hexagon SDK、HexKL、Android 等外部组件归各自权利人所有，按其许可使用。
