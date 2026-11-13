#!/usr/bin/env python3
"""Run the device app checker against a partial CPU model of the DSP RPC."""
from pathlib import Path
import os
import subprocess
import tempfile

here = Path(__file__).resolve().parent
root = here.parents[1]
sdk = Path(os.environ.get('HEXAGON_SDK_ROOT', root / 'deps/hexagon-sdk/6.3.0.0'))
src = here.parent
source = (src / 'qnpu_fused.c').read_text()
source = source.replace('#define _GNU_SOURCE\n', '#ifndef _GNU_SOURCE\n#define _GNU_SOURCE\n#endif\n', 1)
old = 'static int prep_pool_pin(int cpu) {\n    return pin_current_thread_rc(cpu);\n}'
assert source.count(old) == 1
source = source.replace(old, 'static int prep_pool_pin(int cpu) { (void)cpu; return 0; }')
with tempfile.TemporaryDirectory(prefix='prl-deep4-app-check-model-') as name:
    tmp = Path(name)
    (tmp / 'adapter.c').write_text(source)
    arm_gen = tmp / 'arm_gen'
    gen_fused = tmp / 'gen_fused'
    arm_gen.mkdir()
    gen_fused.mkdir()
    qaic = str(sdk / 'ipc/fastrpc/qaic/bin/qaic')
    subprocess.run([qaic, str(root / 'dsp/compound/compound.idl'), '-o', str(arm_gen)], check=True)
    subprocess.run([qaic, str(root / 'dsp/fused/fused.idl'), '-o', str(gen_fused)], check=True)
    cmd = [os.environ.get('CC', 'cc'), '-O2', '-std=gnu11', '-pthread',
           '-D_GNU_SOURCE', '-DPRL_BATCH_ATTEMPTS=32', '-DBLAKE3_USE_NEON=1',
           '-Dprl_compound_batch_run=prl_compound_bytefold_batch_run',
           '-DBLAKE3_NO_SSE2', '-DBLAKE3_NO_SSE41', '-DBLAKE3_NO_AVX2', '-DBLAKE3_NO_AVX512',
           '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
           '-I', str(tmp), '-I', str(here / 'host_neon'), '-I', str(src),
           '-I', str(src / 'blake3'), '-I', str(here), '-I', str(arm_gen),
           '-I', str(root / 'dsp/fused'), '-I', str(gen_fused),
           '-I', str(sdk / 'incs'), '-I', str(sdk / 'incs/stddef'),
           '-I', str(sdk / 'ipc/fastrpc/rpcmem/inc'), str(src / 'app_check.c'),
           str(here / 'app_check_mock_rpc.c'), str(tmp / 'adapter.c'), str(src / 'pearl.c'),
           str(src / 'blake3/blake3.c'), str(src / 'blake3/blake3_dispatch.c'),
           str(src / 'blake3/blake3_portable.c'), '-lm', '-ldl', '-o', str(tmp / 'check')]
    subprocess.run(cmd, check=True)
    env = os.environ.copy()
    env['PRL_PERSIST_MAP'] = '0'
    for mode in ('0', '1'):
        env['PRL_EXPECT_BYTEFOLD_EARLY'] = mode
        subprocess.run([str(tmp / 'check')], check=True, timeout=240, env=env)
