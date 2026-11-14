# SM8750 — Snapdragon 8 Elite（Hexagon v79 cDSP）

`scripts/build-dsp.sh` 以 `PRL_SOC=SM8750`（默认）构建出的 DSP 载荷输出目录：

```
PRL_SOC=SM8750 bash scripts/build-dsp.sh
```

产物（二进制，不入库）：

- `libprl_fused_fused_skel.so` — fused skeleton（自检/回退路径）
- `libprl_compound_bytefold_batch_skel.so` — compound 生产内核，本 SoC 的最优编排
  （v79 cDSP 有 8 个 HVX context：producer + async stage + 4 fold workers）

`scripts/build-apk.sh` 在打 release APK 时自动把本目录的载荷以
`lib*_SM8750.so` 后缀 pickup 进 APK；App 自检在 SM8750/sun 机型上选择本组。

编排旋钮（PRL_V75_* 只作用于 SM8650；本目录暂无额外旋钮）。
