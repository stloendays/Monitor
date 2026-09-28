import sys, os, time, glob, shutil, tempfile
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "hub"))
import pythoncom; pythoncom.CoInitialize()
import monitor_hub as h
d = tempfile.mkdtemp(prefix="hub_setup_test_")
body = h.REQUEST_TEMPLATE.replace("项目名称：", "项目名称：总台提交通道测试").replace("项目目录（本机路径）：", "项目目录（本机路径）：" + d).replace(
    "备注：", "备注：这是总台提交通道的测试。只读 monitor_hub_README.md 第 9 节，然后在办理报告里写一句“提交通道测试通过”和 NEEDS_USER: none。不要创建监控、不要改登记表、不要启动或修改任何任务。")
name = h.App._submit_request(None, body, d) if hasattr(h, "App") else h.Hub._submit_request(None, body, d)
stamp = name[len("hub-setup-"):]
print("launched", name)
for i in range(60):
    time.sleep(10)
    st = h.detach_state(name, h.probe()["procs"])
    if st != "running": break
print("job state:", st)
rep = h.read_text(os.path.join(h.REQ_DIR, stamp + "_report.md"))
print("report:", rep.strip()[:300])
s = h.snapshot(h.SETUP_PROJECT, h.probe())
print("setup snapshot:", s["health"], s["table"]["rows"], [t["state"] for t in s["takeovers"]])
print("transcript tail:", "\n".join(h.cs.render_stream(h.read_text(os.path.join(h.JOBROOT, name, "output.log"))))[-500:])
for f in glob.glob(os.path.join(h.REQ_DIR, stamp + "_*")): os.remove(f)
shutil.rmtree(os.path.join(h.JOBROOT, name), ignore_errors=True); shutil.rmtree(d, ignore_errors=True)
print("cleaned; registry projects:", [p["id"] for p in h.load_registry()["projects"]])
