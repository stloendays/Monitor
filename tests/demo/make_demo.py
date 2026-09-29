"""Build a self-contained demo data set for the monitor hub (no real monitors, no real jobs).

    python tests/demo/make_demo.py [OUT_DIR]          default: %TEMP%\\monitor-hub-demo

Then run the hub on it:
    set MONITOR_HUB_REGISTRY=<OUT_DIR>\\demo_projects.json
    set MONITOR_HUB_DATA=<OUT_DIR>\\hubdata
    set MONITOR_HUB_NO_DISCOVERY=1
    pythonw hub/monitor_hub.py            (or: python hub/monitor_hub.py --dump)
"""
import datetime as dt
import json
import os
import sys

OUT = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.environ.get("TEMP", "."), "monitor-hub-demo")
NOW = dt.datetime.now().replace(microsecond=0)


def iso(minutes_ago=0):
    return (NOW - dt.timedelta(minutes=minutes_ago)).isoformat()


def write(path, text):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8") as f:
        f.write(text)


def transcript(path, steps, final, is_error=False, minutes=3.2, cost=0.41):
    """A stream-json transcript in the format of `claude -p --output-format stream-json --verbose`."""
    lines = [dict(type="system", subtype="init", session_id="00000000-demo-0000-0000-%012d" % (abs(hash(path)) % 10 ** 12),
                  cwd=os.path.dirname(path), model="claude-opus-5-5", tools=["Bash", "Read", "Grep"])]
    for tool, arg, result in steps:
        key = "command" if tool == "Bash" else "file_path"
        lines.append(dict(type="assistant", message=dict(content=[dict(type="tool_use", id="t", name=tool, input={key: arg})])))
        lines.append(dict(type="user", message=dict(content=[dict(type="tool_result", tool_use_id="t", content=result)])))
    lines.append(dict(type="assistant", message=dict(content=[dict(type="text", text=final)])))
    lines.append(dict(type="result", subtype="success" if not is_error else "error", is_error=is_error, result=final,
                      duration_ms=int(minutes * 60000), total_cost_usd=cost))
    write(path, "\n".join(json.dumps(x, ensure_ascii=False) for x in lines) + "\n")


def status(folder, **kw):
    write(os.path.join(folder, "hub_status.json"), json.dumps(dict(updated=iso(4), error="", **kw), ensure_ascii=False, indent=1))


def main():
    a, b, c = (os.path.join(OUT, x) for x in ("local_training", "hpc_batch", "param_scan"))
    # A: a local run in progress, nothing to do
    status(a, headline="训练在跑，没有需要你处理的事。", summary="", done=False, attention=[], working="",
           table=dict(cols=["任务", "状态", "进度", "最近检查点", "预计剩余"],
                      rows=[["fold1", "完成", "100%", "2 小时前", "—"], ["fold2", "运行中", "62%", "3 分钟前", "1.4 小时"],
                            ["fold3", "排队", "0%", "—", "—"]], tags=["done", "run", "queue"],
                      row_meta=[
                          dict(task_id="fold1", open_path=a, log=os.path.join(a, "actions.log"), params=dict(fold=1, batch_size=64)),
                          dict(task_id="fold2", open_path=a, log=os.path.join(a, "actions.log"), params=dict(fold=2, batch_size=32, grad_accum=2)),
                          dict(task_id="fold3", open_path=a, params=dict(fold=3, batch_size=64)),
                      ]),
           notes=["GPU 占用 3 / 8"], next=(NOW + dt.timedelta(minutes=11)).isoformat())
    write(os.path.join(a, "results.md"), "# 结果汇总（进行中）\n\n| fold | 验证集 R² | n |\n|---|---|---|\n| fold1 | 0.912 | 1200 |\n")
    write(os.path.join(a, "actions.log"), "%s  RESTART fold2 (#1): checkpoint resume\n%s  fold1 finished, results.md updated\n" % (iso(95), iso(30)))
    transcript(os.path.join(a, "claude_takeover_%s.jsonl" % (NOW - dt.timedelta(minutes=95)).strftime("%Y%m%d_%H%M")),
               [("Bash", "tail -n 20 logs/fold2.log", "RuntimeError: CUDA out of memory (tried to allocate 2.1 GiB)"),
                ("Read", os.path.join(a, "config.yaml"), "batch_size: 64  grad_accum: 1"),
                ("Bash", "python train.py --fold 2 --resume --batch-size 32 --grad-accum 2", "resumed from step 18000")],
               "fold2 显存不足而停止，已按规程把 batch 减半、梯度累积加倍（等效 batch 不变），从第 18000 步续训。")
    # B: an HPC batch with one item that needs the user
    status(b, headline="有 1 项需要你决定（见下）。", summary="", done=False,
           attention=["slab_O_fcc：第 2 次 NELM 用满，按规程不再自动重投。后台 Claude 给了两个方案（换混合参数 / 从上一步 CONTCAR 重启），请选一个。"],
           working="",
           table=dict(cols=["体系", "作业号", "状态", "进度", "E (eV)", "本次操作"],
                      rows=[["slab_clean", "—", "完成", "最终单点", "-247.31718", ""],
                            ["slab_CO_top", "—", "完成", "最终单点", "-262.90412", ""],
                            ["slab_CO_bridge", "100245", "R 12:40:05", "离子步 31，Fmax 0.041", "-262.8801", ""],
                            ["slab_CO_hollow", "100246", "R 09:02:11", "SCF 18", "-262.7755", ""],
                            ["slab_O_fcc", "—", "NELM 停", "离子步 14", "-253.1180", "未重投（第 2 次）"],
                            ["slab_O_hcp", "100248", "Q", "排队中", "—", ""],
                            ["slab_OH_top", "100249", "Q", "排队中", "—", ""],
                            ["slab_H_fcc", "—", "完成", "最终单点", "-250.66703", ""]],
                      tags=["done", "done", "run", "run", "bad", "queue", "queue", "done"]),
           notes=["scratch 配额：78.5 / 500 G（15.7%）"], next=(NOW + dt.timedelta(hours=4, minutes=40)).isoformat())
    write(os.path.join(b, "SUBMISSION.md"), "## %s 自动监控\n- slab_H_fcc 最终单点完成：E0 = -250.66703 eV\n\n## %s 自动监控\n- slab_O_fcc 第 2 次 NELM 用满，停止自动重投\n" % (iso(300), iso(4)))
    transcript(os.path.join(b, "claude_takeover_%s.jsonl" % (NOW - dt.timedelta(minutes=3)).strftime("%Y%m%d_%H%M")),
               [("Bash", "ssh <host> 'grep -c F= slab_O_fcc/OSZICAR; tail -n 3 slab_O_fcc/OSZICAR'", "14\nDAV: 200  -0.25311803E+03  -0.412E-02"),
                ("Read", os.path.join(b, "SUBMISSION.md"), "第 1 次 NELM：已从 WAVECAR 续算")],
               "slab_O_fcc 连续两次 SCF 用满 200 步，dE 在 1e-3 附近振荡。规程不允许第 3 次自动重投。两个方案：\n"
               "A. ALGO=All + AMIX 0.1 从当前 WAVECAR 重启；B. 从第 12 步 CONTCAR 重启并保持原参数。需要你选。")
    transcript(os.path.join(b, "claude_takeover_%s.jsonl" % (NOW - dt.timedelta(hours=26)).strftime("%Y%m%d_%H%M")),
               [("Bash", "ssh <host> 'qstat -u <user>'", "100240 slab_CO_top R 47:58:12")],
               "You've hit your session limit · resets 10:10pm", is_error=True, minutes=0.3, cost=0.05)
    # C: finished
    status(c, headline="参数扫描全部完成，结果表已写好。", summary="", done=True, attention=[], working="",
           table=dict(cols=["参数", "状态", "结果"], rows=[["U = 3 eV", "完成", "-41.21"], ["U = 4 eV", "完成", "-41.53"], ["U = 5 eV", "完成", "-41.80"]],
                      tags=["done", "done", "done"]),
           notes=[], next="", results=[dict(label="扫描结果表", path=os.path.join(c, "scan_table.md"))])
    write(os.path.join(c, "scan_table.md"), "# 参数扫描结果（n = 3 / 3，失败 0）\n\n| U (eV) | E0 (eV) |\n|---|---|\n| 3 | -41.21 |\n| 4 | -41.53 |\n| 5 | -41.80 |\n")

    def project(pid, name, area, folder, interval, **extra):
        return dict(id=pid, name=name, area=area, adapter="generic", dir=folder,
                    runner=dict(kind="none", interval_min=interval), runner_text="演示数据（没有真实的监控进程），%s" % (
                        "每 %d 分钟" % interval if interval < 60 else "每 %d 小时" % (interval // 60)),
                    status_json=os.path.join(folder, "hub_status.json"),
                    takeovers=dict(kind="glob", pattern=os.path.join(folder, "claude_takeover_*.jsonl")),
                    qa_cwd=folder, qa_sources=[folder], claude_config_dir=None, **extra)
    reg = dict(_help=["监控总台演示数据，由 tests/demo/make_demo.py 生成。"], projects=[
        project("demo-local", "示例 · 本机模型训练", "本机 GPU · 3 个 fold", a, 15, log=os.path.join(a, "actions.log"),
                results=[dict(label="结果汇总", path=os.path.join(a, "results.md"))]),
        project("demo-hpc", "示例 · 表面吸附能计算", "计算化学 · HPC PBS", b, 300, log=os.path.join(b, "SUBMISSION.md"),
                results=[dict(label="结果表", path=os.path.join(b, "results", "batch_table.md"))],
                live_query=dict(label="集群队列（演示）", cmd=[sys.executable, "-c",
                                "print('Job ID   Name            S  Elap\\n100245   slab_CO_bridge  R  12:40\\n100246   slab_CO_hollow  R  09:02\\n100248   slab_O_hcp      Q  --')"])),
        project("demo-done", "示例 · 参数扫描", "本机 · 已完成", c, 60)])
    write(os.path.join(OUT, "demo_projects.json"), json.dumps(reg, ensure_ascii=False, indent=2))
    os.makedirs(os.path.join(OUT, "hubdata", "requests"), exist_ok=True)
    print(OUT)


if __name__ == "__main__":
    main()
