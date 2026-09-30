import os
import sys
import tempfile

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

    with tempfile.TemporaryDirectory() as d:
        script = os.path.join(d, "monitor__ceox-rh__hpc__300m.py")
        with open(script, "w", encoding="utf-8") as f:
            f.write("MONITOR_META = {\n"
                    "  'project_id': 'ceox-rh',\n"
                    "  'scope': 'hpc',\n"
                    "  'interval_min': 300,\n"
                    "  'display_name': 'CeOx/Rh HPC monitor',\n"
                    "  'param_keys': ['ENCUT_eV', 'ISMEAR', 'SIGMA_eV']\n"
                    "}\n"
                    "raise RuntimeError('must never execute')\n")
        meta = naming.read_monitor_meta(script)
        assert meta["display_name"] == "CeOx/Rh HPC monitor"
        ident = naming.parse_monitor_identity("legacy-task", f'python "{script}"')
        assert ident["display_name"] == "CeOx/Rh HPC monitor"
        assert ident["meta"]["param_keys"] == ["ENCUT_eV", "ISMEAR", "SIGMA_eV"]

    print("naming tests passed")


if __name__ == "__main__":
    main()
