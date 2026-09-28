"""Open the hub on the demo data and step through its pages; tests/capture_pages.ps1 screenshots each step.

    python tests/demo/make_demo.py
    pwsh -File tests/capture_pages.ps1 -OutDir docs/images      (in a second shell, or as a background job)
    python tests/demo_tour.py
"""
import os
import subprocess
import sys

DEMO = os.path.join(os.environ.get("TEMP", "."), "monitor-hub-demo")
subprocess.run([sys.executable, os.path.join(os.path.dirname(os.path.abspath(__file__)), "demo", "make_demo.py"), DEMO], check=True)
os.environ.update(MONITOR_HUB_REGISTRY=os.path.join(DEMO, "demo_projects.json"), MONITOR_HUB_DATA=os.path.join(DEMO, "hubdata"),
                  MONITOR_HUB_NO_DISCOVERY="1")
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "hub"))
import monitor_hub as h  # noqa: E402

marker = os.path.join(os.environ["TEMP"], "hub_step.txt")
app = h.Hub()


def dialog():
    app._new_request()
    w = [x for x in app.winfo_children() if str(x).startswith(".!toplevel")][-1]
    t = [c for c in w.winfo_children()[2].winfo_children() if c.winfo_class() == "Text"][0]
    t.delete("1.0", "end")
    t.insert("1.0", h.REQUEST_EXAMPLE)


steps = [("overview", lambda: app._select(None)),
         ("project_progress", lambda: app._select("demo-hpc")),
         ("project_takeover", lambda: (app._select("demo-hpc"), app.nb.select(1))),
         ("project_results", lambda: (app._select("demo-done"), app.nb.select(2))),
         ("project_live", lambda: (app._select("demo-hpc"), app.nb.select(app.t5), app._live_query())),
         ("new_request", dialog)]


def wait_data():
    if app.shown_version < 1:
        app.after(500, wait_data)
    else:
        app.after(1500, lambda: run(0))


def run(i):
    if i >= len(steps):
        open(marker, "w").write("END")
        app.after(1500, app.destroy)
        return
    name, fn = steps[i]
    fn()
    app.after(4000, lambda: (open(marker, "w").write(name), app.after(3000, lambda: run(i + 1))))


open(marker, "w").write("start")
app.after(500, wait_data)
app.mainloop()
