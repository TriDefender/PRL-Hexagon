# SM8650 — Snapdragon 8 Gen 3（Hexagon v75 cDSP）

`scripts/build-dsp.sh` 以 `PRL_SOC=SM8650` 构建出的 DSP 载荷输出目录：

```
PRL_SOC=SM8650 bash scripts/build-dsp.sh
```

产物（二进制，不入库）：

- `libprl_fused_fused_skel.so` — fused skeleton（自检/回退路径）
- `libprl_compound_bytefold_batch_skel.so` — compound 生产内核，本 SoC 的最优编排

v75 cDSP 只有 4 个 HVX context（v79 是 8），本目录内核的编排由以下旋钮控制，
必须满足 producer +（可选 stage 线程）+ fold workers ≤ 4：

- `PRL_V75_WORKERS`（默认 2，配 stage 线程）
- `PRL_V75_ASYNC_STAGE=0` 时 producer 内联 staging，省出的 context 换一个 worker
- `PRL_V75_HMX_CLOCK` 覆盖 PRL_HMX_CLOCK_POLICY

`scripts/build-apk.sh` 自动把本目录载荷以 `lib*_SM8650.so` 后缀 pickup 进 APK；
App 自检在 SM8650/pineapple 机型上选择本组。

不兼容项与适配过程见 `docs/validation/8gen3-adapt-20261114/README.md`。
