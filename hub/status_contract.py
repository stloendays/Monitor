"""Validation helpers for generic hub_status.json files.

This intentionally uses only the Python standard library so monitors can be checked
on a clean Windows machine without adding a runtime dependency on jsonschema.
"""
from __future__ import annotations

import datetime as dt

ALLOWED_TAGS = {"done", "run", "queue", "bad", "other", ""}
SCALAR = (str, int, float)


def _is_number_or_string(value):
    return isinstance(value, SCALAR) and not isinstance(value, bool)


def validate_status(data):
    """Return {"errors": [...], "warnings": [...]} for a parsed hub_status object."""
    errors, warnings = [], []
    if not isinstance(data, dict):
        return {"errors": ["根对象必须是 JSON object"], "warnings": []}

    if "schema_version" in data and (not isinstance(data["schema_version"], int) or isinstance(data["schema_version"], bool) or data["schema_version"] < 1):
        errors.append("schema_version 必须是 >= 1 的整数")

    updated = data.get("updated")
    if not isinstance(updated, str) or not updated.strip():
        errors.append("updated 必须是非空 ISO 8601 字符串")
    else:
        try:
            dt.datetime.fromisoformat(updated)
        except ValueError:
            errors.append("updated 不是有效 ISO 8601 时间")

    if not isinstance(data.get("headline"), str) or not data.get("headline", "").strip():
        errors.append("headline 必须是非空字符串")

    if "done" in data and not isinstance(data["done"], bool):
        errors.append("done 必须是 boolean")

    for key in ("working", "error", "next", "summary"):
        if key in data and data[key] is not None and not isinstance(data[key], str):
            errors.append(f"{key} 必须是字符串")

    for key in ("attention", "notes"):
        if key in data:
            value = data[key]
            if not isinstance(value, list) or any(not isinstance(x, str) for x in value):
                errors.append(f"{key} 必须是字符串数组")

    table = data.get("table")
    if not isinstance(table, dict):
        errors.append("table 必须是 object")
        return {"errors": errors, "warnings": warnings}

    cols, rows = table.get("cols"), table.get("rows")
    if not isinstance(cols, list) or any(not isinstance(x, str) for x in cols):
        errors.append("table.cols 必须是字符串数组")
        cols = []
    if not isinstance(rows, list):
        errors.append("table.rows 必须是二维数组")
        rows = []

    for i, row in enumerate(rows):
        if not isinstance(row, list):
            errors.append(f"table.rows[{i}] 必须是数组")
            continue
        if cols and len(row) != len(cols):
            errors.append(f"table.rows[{i}] 有 {len(row)} 列，但 cols 定义了 {len(cols)} 列")
        for j, cell in enumerate(row):
            if not _is_number_or_string(cell):
                errors.append(f"table.rows[{i}][{j}] 只能是字符串或数字")

    tags = table.get("tags")
    if tags is not None:
        if not isinstance(tags, list):
            errors.append("table.tags 必须是数组")
        else:
            if len(tags) != len(rows):
                warnings.append(f"table.tags 有 {len(tags)} 项，但 rows 有 {len(rows)} 行；总台会自动补齐/截断")
            for i, tag in enumerate(tags):
                if tag not in ALLOWED_TAGS:
                    errors.append(f"table.tags[{i}]={tag!r} 不在允许集合中")

    row_meta = table.get("row_meta")
    if row_meta is not None:
        if not isinstance(row_meta, list):
            errors.append("table.row_meta 必须是数组")
        else:
            if len(row_meta) != len(rows):
                warnings.append(f"table.row_meta 有 {len(row_meta)} 项，但 rows 有 {len(rows)} 行；总台会自动补齐/截断")
            seen = set()
            for i, meta in enumerate(row_meta):
                if not isinstance(meta, dict):
                    errors.append(f"table.row_meta[{i}] 必须是 object")
                    continue
                task_id = meta.get("task_id")
                if task_id is not None:
                    if not isinstance(task_id, str) or not task_id.strip():
                        errors.append(f"table.row_meta[{i}].task_id 必须是非空字符串")
                    elif task_id in seen:
                        warnings.append(f"task_id {task_id!r} 重复；刷新后任务选择可能不稳定")
                    else:
                        seen.add(task_id)
                elif meta:
                    warnings.append(f"table.row_meta[{i}] 没有 task_id；建议为可点击任务提供稳定 task_id")
                for key in ("host", "open_path", "path", "workdir", "log", "result", "script", "command"):
                    if key in meta and meta[key] is not None and not isinstance(meta[key], str):
                        errors.append(f"table.row_meta[{i}].{key} 必须是字符串")
                if "params" in meta and not isinstance(meta["params"], dict):
                    errors.append(f"table.row_meta[{i}].params 必须是 object")

    results = data.get("results")
    if results is not None:
        if not isinstance(results, list):
            errors.append("results 必须是数组")
        else:
            for i, item in enumerate(results):
                if not isinstance(item, dict):
                    errors.append(f"results[{i}] 必须是 object")
                    continue
                if not isinstance(item.get("path"), str) or not item.get("path", "").strip():
                    errors.append(f"results[{i}].path 必须是非空字符串")
                if "label" in item and not isinstance(item["label"], str):
                    errors.append(f"results[{i}].label 必须是字符串")

    return {"errors": errors, "warnings": warnings}
