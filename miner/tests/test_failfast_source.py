#!/usr/bin/env python3
"""Static guards tying the fault-injection harness to the production worker."""
from pathlib import Path

here = Path(__file__).resolve().parent
main = (here.parent / "main.c").read_text()
worker_start = main.index("static void *qnpu_miner_thread(void *arg)")
worker_end = main.index("static void *miner_thread(void *arg)", worker_start)
worker = main[worker_start:worker_end]

pipe = worker.index("const int compound_rc = pr_qnpu_compound_pipe(g, nonce);")
fatal_if = worker.index("if (compound_rc != 0)", pipe)
fatal_store = worker.index("atomic_store(&ms->fatal, 1u);", fatal_if)
stop_store = worker.index("atomic_store(&ms->stop, 1u);", fatal_store)
clear_last = worker.index("has_last = 0;", stop_store)
break_at = worker.index("break;", clear_last)
result_read = worker.index("pr_qnpu_compound_result(")
fused_rpc = worker.index("pr_qnpu_attempt_pipe2(")
assert pipe < fatal_if < fatal_store < stop_store < clear_last < break_at < result_read < fused_rpc
assert "unavailable after RPC/preparation error" not in worker

loop_end = worker.index("    /* drain: fold the last queued attempt and submit its hits */", break_at)
assert "pr_qnpu_drain(g, coords, digests)" in worker[loop_end:]
assert worker.index("while (!atomic_load(&ms->stop))") < break_at < loop_end
result_fail = worker.index("if (item_hits < 0)")
result_fail_body = worker[result_fail:worker.index("qnpu_submit_result", result_fail)]
assert "atomic_store(&ms->fatal, 1u);" in result_fail_body
assert "atomic_store(&ms->stop, 1u);" in result_fail_body
assert "compound_result_failed = 1;" in result_fail_body
assert worker.index("if (compound_result_failed) break;") < fused_rpc

adapter = (here.parent / "qnpu_fused.c").read_text()
drain_start = adapter.index("int pr_qnpu_drain(pr_qnpu_ctx *c,")
drain_end = adapter.index("const int8_t *pr_qnpu_host_a", drain_start)
drain = adapter[drain_start:drain_end]
assert "return_pending(c,coords,digests)" in drain
assert "prl_compound_batch_run(" not in drain
assert "prl_fused_fused_run_hash(" not in drain

status_fn = main.index("static int miner_fatal_exit_status(")
status_end = main.index("static void next_seed", status_fn)
assert "atomic_load(&ms->fatal) ? 2 : 0" in main[status_fn:status_end]
pool_loop = main.index("static int run_pool(")
pool_end = main.index("static int run_offline(", pool_loop)
pool = main[pool_loop:pool_end]
assert "return exit_status;" in pool
assert pool.index("miner_fatal_exit_status(&ms)") > pool.index("while (!atomic_load(&ms.stop))")
print("FAILFAST_SOURCE_PASSED production_worker=1 compound_failure_break_before_results_and_fused=1 drain_unconditional=1 drain_no_rpc=1 fatal_pool_exit=2")
