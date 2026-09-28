import os
import sys, time, subprocess
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "hub"))
import pythoncom; pythoncom.CoInitialize()
import monitor_hub as h
t0 = time.time(); si = h.probe(); print("probe %.2fs err=%s" % (time.time() - t0, si["error"]))
for n, t in si["tasks"].items(): print("  task", n, t["state"], t["interval"], t["last"], t["next"], hex(t["result"]))
print("  procs:", len(si["procs"]), [p["name"] for p in si["procs"]][:8])
# temp task for the management actions
subprocess.run(["pwsh", "-NoProfile", "-Command",
  "$a = New-ScheduledTaskAction -Execute 'conhost.exe' -Argument '--headless powershell.exe -NoProfile -Command Start-Sleep 2'; "
  "$t = New-ScheduledTaskTrigger -Once -At (Get-Date).AddHours(3) -RepetitionInterval (New-TimeSpan -Hours 5); "
  "Register-ScheduledTask -TaskName 'hub-com-test-monitor' -Action $a -Trigger $t -Force | Out-Null"], check=True, **h.hidden())
def state():
    t = h.probe()["tasks"].get("hub-com-test-monitor"); return (t["state"], t["interval"], t["next"]) if t else None
print("created:", state())
h.schtask_do("hub-com-test-monitor", "interval", 90); print("interval 90 ->", state())
h.schtask_do("hub-com-test-monitor", "pause"); print("pause ->", state())
h.schtask_do("hub-com-test-monitor", "resume"); print("resume ->", state())
h.schtask_do("hub-com-test-monitor", "run"); time.sleep(0.5); print("run ->", state())
time.sleep(4); t = h.probe()["tasks"]["hub-com-test-monitor"]; print("after run: last=%s result=%s" % (t["last"], hex(t["result"])))
subprocess.run(["pwsh", "-NoProfile", "-Command", "Unregister-ScheduledTask -TaskName 'hub-com-test-monitor' -Confirm:$false"], **h.hidden())
print("cleaned:", state())
