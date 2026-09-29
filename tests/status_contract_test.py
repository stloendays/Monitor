import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "hub"))
import status_contract as sc


def good():
    return {
        "updated": "2026-09-29T10:00:00",
        "headline": "作业正常。",
        "done": False,
        "table": {
            "cols": ["任务", "状态"],
            "rows": [["a", "运行中"], ["b", "完成"]],
            "tags": ["run", "done"],
            "row_meta": [
                {"task_id": "a", "job_id": "1001", "params": {"ENCUT_eV": 450}},
                {"task_id": "b", "result": "D:/results/b.md"},
            ],
        },
        "attention": [],
        "notes": [],
        "working": "",
        "results": [{"label": "结果", "path": "D:/results/all.md"}],
        "error": "",
    }


def main():
    out = sc.validate_status(good())
    assert out == {"errors": [], "warnings": []}, out

    x = good()
    x["table"]["rows"][0] = ["a"]
    x["table"]["row_meta"][1]["task_id"] = "a"
    out = sc.validate_status(x)
    assert any("cols" in e for e in out["errors"]), out
    assert any("重复" in w for w in out["warnings"]), out

    x = good()
    x["updated"] = "not-a-time"
    x["table"]["row_meta"] = [{"task_id": "a"}]
    out = sc.validate_status(x)
    assert any("ISO 8601" in e for e in out["errors"]), out
    assert any("row_meta" in w for w in out["warnings"]), out

    print("status contract tests passed")


if __name__ == "__main__":
    main()
