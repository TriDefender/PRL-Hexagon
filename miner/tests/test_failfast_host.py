#!/usr/bin/env python3
"""Exercise the adapter and noise helpers with software NEON and fake DSP RPC."""
from pathlib import Path
import os
import subprocess
import tempfile

here = Path(__file__).resolve().parent
miner = here.parent
root = miner.parent
sdk = Path(os.environ.get('HEXAGON_SDK_ROOT', root / 'deps/hexagon-sdk/6.3.0.0'))
qaic = str(sdk / 'ipc/fastrpc/qaic/bin/qaic')
source = (miner / 'qnpu_fused.c').read_text()
source = source.replace('#define _GNU_SOURCE\n', '#ifndef _GNU_SOURCE\n#define _GNU_SOURCE\n#endif\n', 1)
old_pin = 'static int prep_pool_pin(int cpu) {\n    return pin_current_thread_rc(cpu);\n}'
assert source.count(old_pin) == 1
source = source.replace(old_pin, 'static int prep_pool_pin(int cpu) { (void)cpu; return 0; }')
result_marker = "int pr_qnpu_compound_result(pr_qnpu_ctx *c, uint32_t attempt,\n                            uint64_t *nonce_out, uint64_t *coords,\n                            uint8_t *digests) {\n    if (!c) return -1;"
assert source.count(result_marker) == 1
source = source.replace(result_marker, result_marker.replace("    if (!c) return -1;", "    extern unsigned host_compound_result_calls;\n    extern int host_force_result_failure;\n    ++host_compound_result_calls;\n    if (host_force_result_failure) { host_force_result_failure = 0; return -1; }\n    if (!c) return -1;"))
drain_marker = "int pr_qnpu_drain(pr_qnpu_ctx *c, uint64_t *coords, uint8_t *digests) {\n    if (!c) return 0;"
assert source.count(drain_marker) == 1
source = source.replace(drain_marker, "int pr_qnpu_drain(pr_qnpu_ctx *c, uint64_t *coords, uint8_t *digests) {\n    extern unsigned host_drain_calls;\n    extern unsigned host_rpc_calls_at_drain_start, host_rpc_calls_at_drain_end;\n    extern int rpc_calls;\n    ++host_drain_calls;\n    host_rpc_calls_at_drain_start = (unsigned)rpc_calls;\n    if (!c) { host_rpc_calls_at_drain_end = (unsigned)rpc_calls; return 0; }")
old_return = "    return return_pending(c,coords,digests);\n}"
assert source.count(old_return) == 1
source = source.replace(old_return, "    int pending_hits = return_pending(c,coords,digests);\n    host_rpc_calls_at_drain_end = (unsigned)rpc_calls;\n    return pending_hits;\n}")
with tempfile.TemporaryDirectory(prefix='prl-deep4-failfast-') as name:
    tmp = Path(name)
    (tmp / 'actual_qnpu.inc').write_text(source)
    arm_gen = tmp / 'arm_gen'
    gen_fused = tmp / 'gen_fused'
    arm_gen.mkdir()
    gen_fused.mkdir()
    subprocess.run([qaic, str(root / 'dsp/compound/compound.idl'), '-o', str(arm_gen)], check=True)
    subprocess.run([qaic, str(root / 'dsp/fused/fused.idl'), '-o', str(gen_fused)], check=True)
    cmd = [os.environ.get('CC', 'cc'), '-O2', '-std=gnu11', '-pthread',
           '-DWITH_QNPU', '-DPRL_BATCH_ATTEMPTS=32', '-DBLAKE3_USE_NEON=1',
           '-Dprl_compound_batch_run=prl_compound_bytefold_batch_run',
           '-DBLAKE3_NO_SSE2', '-DBLAKE3_NO_SSE41', '-DBLAKE3_NO_AVX2', '-DBLAKE3_NO_AVX512',
           '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
           '-ffunction-sections', '-fdata-sections', '-Wl,--gc-sections',
           '-I', str(tmp), '-I', str(here / 'host_neon'), '-I', str(miner),
           '-I', str(miner / 'blake3'), '-I', str(arm_gen), '-I', str(gen_fused),
           '-I', str(sdk / 'incs'),
           '-I', str(sdk / 'incs/stddef'), '-I', str(sdk / 'ipc/fastrpc/rpcmem/inc'),
           str(here / 'adapter_host.c'), str(miner / 'pearl.c'), str(miner / 'mine.c'), str(miner / 'proof.c'),
           str(miner / 'blake3/blake3.c'), str(miner / 'blake3/blake3_dispatch.c'),
           str(miner / 'blake3/blake3_portable.c'), '-lm', '-ldl', '-o', str(tmp / 'check')]
    subprocess.run(cmd, check=True)
    env = os.environ.copy()
    env['PRL_EXPECT_BYTEFOLD_EARLY'] = '0'
    env['PRL_PERSIST_MAP'] = '0'
    subprocess.run([str(tmp / 'check')], check=True, timeout=240, env=env)
