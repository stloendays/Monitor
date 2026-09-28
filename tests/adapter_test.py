import sys, os, json, tempfile, time
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "hub"))
import pythoncom; pythoncom.CoInitialize()
import monitor_hub as h
si = h.probe()
d = tempfile.mkdtemp(prefix="hub_generic_")
st = {"updated": time.strftime("%Y-%m-%dT%H:%M:%S"), "headline": "所有作业正常运行，无需操作。", "summary": "", "done": False,
      "table": {"cols": ["体系", "作业号", "状态"], "rows": [["A", "1", "完成"], ["B", "2", "R 01:00"], ["C", "3", "Q"]], "tags": ["done", "run", "queue"]},
      "attention": [], "working": "", "notes": ["scratch 78 G"], "next": "2026-09-29T02:45:00",
      "results": [{"label": "结果表", "path": os.path.join(d, "table.md")}], "error": ""}
json.dump(st, open(os.path.join(d, "hub_status.json"), "w", encoding="utf-8"), ensure_ascii=False)
p = dict(id="t", name="合成项目", adapter="generic", runner=dict(kind="none"), status_json=os.path.join(d, "hub_status.json"), dir=d)
s = h.snapshot(p, si)
print("generic:", s["health"], "|", s["headline"], "|", s["summary"], "| next", s["next"], "| results", s["results_list"])
st["attention"] = ["B 第 2 次 NELM 用满，要不要换混合参数？"]; json.dump(st, open(os.path.join(d, "hub_status.json"), "w", encoding="utf-8"), ensure_ascii=False)
print("generic+attention:", h.snapshot(p, si)["health"])
st["attention"] = []; st["done"] = True; json.dump(st, open(os.path.join(d, "hub_status.json"), "w", encoding="utf-8"), ensure_ascii=False)
print("generic done:", h.snapshot(p, si)["health"])
st["done"] = False; st["updated"] = "2026-09-20T00:00:00"; json.dump(st, open(os.path.join(d, "hub_status.json"), "w", encoding="utf-8"), ensure_ascii=False)
p["runner"] = dict(kind="none", interval_min=60); print("generic stale:", h.snapshot(p, si)["health"])
for pr in h.load_projects(si):
    s = h.snapshot(pr, si)
    print("%-14s %-9s results=%s | %s" % (pr["id"], s["health"], [(l, os.path.exists(x)) for l, x in s["results_list"]], s.get("problem") or s.get("summary") or s.get("headline")))
