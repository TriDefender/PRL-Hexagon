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
with tempfile.TemporaryDirectory(prefix='prl-compact-adapter-') as name:
    tmp = Path(name)
    (tmp / 'actual_qnpu.inc').write_text(source)
    arm_gen = tmp / 'arm_gen'
    gen_fused = tmp / 'gen_fused'
    arm_gen.mkdir()
    gen_fused.mkdir()
    subprocess.run([qaic, str(root / 'dsp/compound/compound.idl'), '-o', str(arm_gen)], check=True)
    subprocess.run([qaic, str(root / 'dsp/fused/fused.idl'), '-o', str(gen_fused)], check=True)
    cmd = [os.environ.get('CC', 'cc'), '-O2', '-std=gnu11', '-pthread',
           '-D_GNU_SOURCE', '-DPRL_BATCH_ATTEMPTS=32', '-DBLAKE3_USE_NEON=1',
           '-DBLAKE3_NO_SSE2', '-DBLAKE3_NO_SSE41', '-DBLAKE3_NO_AVX2', '-DBLAKE3_NO_AVX512',
           '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
           '-I', str(tmp), '-I', str(here / 'host_neon'), '-I', str(miner),
           '-I', str(miner / 'blake3'), '-I', str(arm_gen), '-I', str(gen_fused),
           '-I', str(sdk / 'incs'),
           '-I', str(sdk / 'incs/stddef'), '-I', str(sdk / 'ipc/fastrpc/rpcmem/inc'),
           str(here / 'adapter_host.c'), str(miner / 'pearl.c'),
           str(miner / 'blake3/blake3.c'), str(miner / 'blake3/blake3_dispatch.c'),
           str(miner / 'blake3/blake3_portable.c'), '-lm', '-ldl', '-o', str(tmp / 'check')]
    subprocess.run(cmd, check=True)
    subprocess.run([str(tmp / 'check')], check=True, timeout=180)
