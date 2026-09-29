import json
import os
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "hub"))
import monitor_hub as mh


def main():
    with tempfile.TemporaryDirectory() as d:
        status_path = os.path.join(d, "status.json")
        data = {
            "updated": mh.now().replace(microsecond=0).isoformat(),
            "rows": [
                {
                    "job": "fold1",
                    "status": "COMPLETE",
                    "checkpoints": 10,
                    "failures": 0,
                    "total": 10,
                    "branch_pushed": True,
                    "params": {"fold": 1},
                },
                {
                    "job": "fold2",
                    "status": "RUNNING",
                    "checkpoints": 4,
                    "failures": 1,
                    "total": 10,
                    "rate_per_h": 2.5,
                    "eta_h": 2.0,
                    "branch_pushed": False,
                    "workdir": d,
                    "params": {"fold": 2, "seed": 43},
                },
            ],
            "live": {
                "fold2": {
                    "checkpoints": 7,
                    "failures": 1,
                    "newest_checkpoint": mh.now().replace(microsecond=0).isoformat(),
                }
            },
            "notify": [],
            "attention": [],
        }
        with open(status_path, "w", encoding="utf-8") as f:
            json.dump(data, f)

        p = {"status_json": status_path}
        runner = {"interval": 15, "running": True}
        snap = mh.adapt_qoi(p, {"procs": []}, runner)
        assert snap["table"]["rows"][1][2] == "7 / 10（70%）", snap
        assert snap["table"]["row_meta"][1]["task_id"] == "fold2"
        assert snap["table"]["row_meta"][1]["params"]["seed"] == 43
        assert snap["summary"].startswith("1/2 个作业已完成；fold2 70%"), snap["summary"]
        assert snap["done"] is False

    print("qoi adapter tests passed")


if __name__ == "__main__":
    main()
