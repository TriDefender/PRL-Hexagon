# Early byte-fold：2026-11-13 验证记录

本次将已验证的 early byte-plane spatial fold 设为 main 默认。固定 deep1、PF0、G16、4 个 DSP workers 和 4 个 ARM prep workers；保留 producer/export、同步、transcript、hash、nonce 与矿池协议。算法只替换两个 consumer fold 调用点。App versionCode=7，versionName=bytefold-early。

| 真实池组 | 顺序 | 有效一秒窗 | 旧 fold TMAC/s | 新 fold TMAC/s | 提升 |
|---|---|---:|---:|---:|---:|
| ABBA | 0/1/1/0 | 78 | 6.43797 | 6.99041 | 8.58% |
| BAAB | 1/0/0/1 | 80 | 6.25915 | 6.91601 | 10.49% |

硬件为 OnePlus PLQ110 / SM8750 / Hexagon v79 cDSP，生产尺寸 M=2048,N=1024,K=2048，每批 32 nonce。两组相隔约 8 小时、同一 boot 和冻结二进制；每腿 30 秒、独立 32 秒截止，排除最初 10 秒及首个 tag 后的 baseline 窗口，只取完整 1.0–1.1 秒窗口，按总 MAC 增量除以总采样时间计算。四个相邻新/旧配对均正；基线漂移分别为 −2.53% 和 +0.90%。起点温度并不匹配，两组应分开解释。

用户确认手机闲置、未充电、持续简单风冷。最高采样 NPU 106.5°C，存在热降频。仅将实验脚本的数字热停止设为 record-only，OEM 温控/DCVS 未改。未证明长时间热稳态；accepted/rejected 均为 0/0，share 提交仍未验证。

新版“不降频”简单外推：仅 Q6 限速的线性假设约 7.77–8.08 TMAC/s；仅 HMX 限速的替代假设约 9.52–9.57 TMAC/s。参考频率 Q6=2112、HMX=1209 MHz，使用合格窗口内每 64 batch 的 pre/post 频率调和均值，按 `modeled_dt = actual_dt * f_h / f_ref` 计算。边界采样不是频率驻留；两个模型不能相乘，也不是实测性能或置信区间。

两种模式的完整驱动均完成 32 nonce 的 commitment、CPU TT/DSP raw TT、全部 524288 digest 和 16384 mask 对拍。两个实际 adapter appchecker 均退出 0，各检查 2 jobs、4 batches、128 attempts、nonce wrap 和 2 次完整 scalar mask，实际尺寸为 M=2048,N=64,K=2048。

原 mode1 完整驱动观察在主机 35 秒超时，设备日志已输出完整 MATCH/PASSED，但没有正常退出状态；该记录保留为不完整观察。另一次独立的 detached observer、25 秒 watchdog、owned suspend blocker 受控验证完成全量对拍并退出 0；旧超时原因未被单独归因。本次不把它作为无异常运行，也未从失败 RPC 读取 payload。

默认 service 为 `prl_compound_bytefold_batch`，库名为 `libprl_compound_bytefold_batch_skel.so`，实际 word63 tag 必须为 `0x42464501`。App 设置 `PRL_EXPECT_BYTEFOLD_EARLY=1`，每个成功 RPC 都检查 family/mode。命令行也需设置该环境变量，并以配套库目录为 CWD 和分号 `ADSP_LIBRARY_PATH` 首项。mode0 仅作旧 fold 对照。

发布仓库从冻结候选导入计算源文件，适配构建与测试目录。重建两个 kernel object 的 SHA-256 与真机已测 object 完全一致；新链接的 DSP/ARM 文件哈希可因链接顺序、路径而不同。`SOURCE_IMPORTS.json` 记录完全一致的文件，`LOCAL_BUILD_REVIEW.json` 区分原设备验证与本次重建。此次未重新在设备验证重建 APK，也未安装 APK、发布 Release 或替换手机上的旧正式载荷。

本次本地构建和主机检查：

```sh
# 设置 HEXAGON_SDK_ROOT、HEXKL_ROOT、ANDROID_NDK_ROOT 后执行
bash scripts/build-dsp.sh
bash scripts/build-miner.sh
python3 -I dsp/compound/test_compact_outer_host.py
python3 -I miner/tests/test_failfast_source.py
python3 -I miner/tests/test_failfast_host.py
python3 -I miner/tests/test_app_check_host.py
```

全部通过，host 故障注入使用 ASan/UBSan 和模拟 RPC，并非硬件证明。首次迁移时 failfast harness 仍引用旧 `../miner_src/main.c` 路径，编译失败；修为 `../main.c` 后通过，首次记录保留。`build_checks/` 包含命令输出。未改融合自检/回退内核。

`ABBA/`、`BAAB/` 保留分析、独立算术复核、逐腿 stats、频率/温度、affinity、电池和退出状态。`miner.txt` 仅省略连接/账户行，保留行数；不含钱包配置或应用 preferences。完整 CPU 对拍及 appchecker 输出在对应 `mode*_full*`、`mode*_app/` 目录。原始试验完整归档仍保留在工作区，本目录是去掉账户信息的发布证据摘录。

测后确认无 owned workload、无临时矿池配置、owned 唤醒锁释放，旧正式/PF0/legacy 库哈希不变。具体记录见 `FINAL_DEVICE_POSTFLIGHT.json`。尚未重做完整阶段 profile，不能给出新的 producer/export/MM 精确占比；抽样 prep 约 10–13 ms、RPC 约 19–21 ms，主要等待仍在 DSP/RPC 段。
