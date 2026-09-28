import sys, time, os, shutil
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "hub"))
import pythoncom; pythoncom.CoInitialize()
import monitor_hub as h
r = dict(kind="detach", name="hub-test-monitor", interval_min=7, workdir=os.environ["TEMP"],
         start_cmd="'interval={interval}' | Out-File -Encoding utf8 $env:TEMP\hub_test_interval.txt; Start-Sleep -Seconds 600")
def st():
    time.sleep(3); si = h.probe(); return h.detach_state(r["name"], si["procs"]), open(os.path.join(os.environ["TEMP"], "hub_test_interval.txt"), encoding="utf-8-sig").read().strip() if os.path.exists(os.path.join(os.environ["TEMP"], "hub_test_interval.txt")) else None
h.detach_do(r, "resume"); print("resume ->", st())
h.detach_do(r, "run"); print("run (restart) ->", st())
h.detach_do(r, "interval", 42); print("interval 42 ->", st())
h.detach_do(r, "pause"); print("pause ->", st())
shutil.rmtree(os.path.join(h.JOBROOT, r["name"]), ignore_errors=True); os.remove(os.path.join(os.environ["TEMP"], "hub_test_interval.txt"))
print("cleaned")
