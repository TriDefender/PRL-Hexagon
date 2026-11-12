#!/usr/bin/env python3
"""Run the real compact outer RPC under host sanitizers with a fake kernel."""
from pathlib import Path
import os
import subprocess
import tempfile

here = Path(__file__).resolve().parent
root = here.parents[1]
sdk = Path(os.environ.get('HEXAGON_SDK_ROOT', root / 'deps/hexagon-sdk/6.3.0.0'))
qaic = str(sdk / 'ipc/fastrpc/qaic/bin/qaic')
source = (here / 'impl.c').read_text()
old = 'static void complete(void) {__asm__ volatile("syncht":::"memory");}'
assert source.count(old) == 1
source = source.replace(old, 'static void complete(void) {host_drain();}')
with tempfile.TemporaryDirectory(prefix='prl-compact-outer-') as name:
    tmp = Path(name)
    (tmp / 'actual_compound.inc').write_text(source)
    (tmp / 'HAP_perf.h').write_text(
        '#include <stdint.h>\nstatic uint64_t HAP_perf_get_qtimer_count(void);\n'
        'static uint64_t HAP_perf_get_pcycles(void);\n')
    gen = tmp / 'gen'
    kernel_gen = tmp / 'kernel_gen'
    gen.mkdir()
    kernel_gen.mkdir()
    subprocess.run([qaic, str(here / 'compound.idl'), '-o', str(gen)], check=True)
    subprocess.run([qaic, str(here / 'snapshot/fused.idl'), '-o', str(kernel_gen)], check=True)
    cmd = [os.environ.get('CC', 'cc'), '-O2', '-std=gnu99', '-pthread',
           '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
           '-I', str(tmp), '-I', str(here), '-I', str(gen), '-I', str(here / 'snapshot'),
           '-I', str(sdk / 'incs'), '-I', str(sdk / 'incs/stddef'),
           '-I', str(sdk / 'ipc/fastrpc/remote/ship/inc'),
           str(here / 'tests/compact_outer_host.c'), '-o', str(tmp / 'check')]
    subprocess.run(cmd, check=True)
    subprocess.run([str(tmp / 'check')], check=True, timeout=60)
