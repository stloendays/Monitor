import json
import os
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "hub"))
import monitor_hub as mh


def main():
    old_req, old_job = mh.REQ_DIR, mh.JOBROOT
    try:
        with tempfile.TemporaryDirectory() as d:
            req = os.path.join(d, "requests")
            jobs = os.path.join(d, "jobs")
            os.makedirs(req)
            os.makedirs(jobs)
            mh.REQ_DIR = req
            mh.JOBROOT = jobs

            stamp = "20260929-130000"
            request = os.path.join(req, stamp + "_request.md")
            report = os.path.join(req, stamp + "_report.md")
            with open(request, "w", encoding="utf-8") as f:
                f.write("【监控任务】\n项目名称：CeOx Phase B\n")
            with open(report, "w", encoding="utf-8") as f:
                f.write("# 办理报告\n已创建监控。\nNEEDS_USER: 请选择 walltime\n")

            job_dir = os.path.join(jobs, "hub-setup-" + stamp)
            os.makedirs(job_dir)
            with open(os.path.join(job_dir, "output.log"), "w", encoding="utf-8") as f:
                f.write(json.dumps({"type": "result", "is_error": False, "result": "setup finished"}, ensure_ascii=False) + "\n")
            with open(os.path.join(job_dir, "exitcode"), "w", encoding="utf-8") as f:
                f.write("0\n")

            snap = mh.adapt_setup({}, {"procs": []}, {})
            assert snap["table"]["rows"][0][1] == "CeOx Phase B", snap
            assert snap["table"]["rows"][0][2] == "办好了，有事要你定", snap
            assert snap["table"]["row_meta"][0]["task_id"] == stamp
            assert snap["table"]["row_meta"][0]["result"] == report
            assert snap["table"]["row_meta"][0]["log"].endswith("output.log")
            assert any("walltime" in x for x in snap["attention"])
            assert len(snap["takeovers"]) == 1

        print("setup adapter tests passed")
    finally:
        mh.REQ_DIR, mh.JOBROOT = old_req, old_job


if __name__ == "__main__":
    main()
