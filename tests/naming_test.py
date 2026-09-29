import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "hub"))
import naming


def main():
    a = naming.parse_monitor_name("monitor__ceox-rh__hpc__300m.py")
    assert a["project"] == "ceox-rh"
    assert a["scope"] == "hpc"
    assert a["interval_min"] == 300

    b = naming.parse_monitor_name("qoi-ext__monitor__local__15m")
    assert b["project"] == "qoi-ext"
    assert b["interval_min"] == 15

    c = naming.parse_monitor_identity(
        "legacy-task",
        r"conhost.exe --headless python D:\Research\monitor__pur-screening__local__30m.py",
    )
    assert c["project"] == "pur-screening"
    assert c["interval_min"] == 30

    assert naming.parse_monitor_name("my-monitor-5h") == {}
    print("naming tests passed")


if __name__ == "__main__":
    main()
