# common — 架构无关载荷

`scripts/build-miner.sh` 的输出目录（二进制，不入库）：

```
bash scripts/build-miner.sh
```

- `libprlminer.so` — 矿工（stratum + Pearl proof + FastRPC 适配），不绑定 DSP 架构
- `app-check` — 实际 compound 内核的 adapter 检查器（M=2048,N=64,K=2048）

各 SoC 的 DSP 载荷在同级 `SM*/` 目录（`scripts/build-dsp.sh`）；
`scripts/build-apk.sh` 从这里取 miner/appcheck、从 SM*/ 取 skeleton 打包 APK。
