from __future__ import annotations

import csv
import math
import os
import subprocess
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Callable

_DEFAULT_EXPORT_THREADS = (
    "GameThread",
    "RenderThread 0",
    "RHIThread",
)
_DEFAULT_INSIGHTS_TIMEOUT_SECONDS = 900


def csv_paths_from_trace(trace_path: Path) -> dict[str, Path]:
    base = trace_path.with_suffix("")
    return {
        "threads": Path(str(base) + "Threads.csv"),
        "thread_scopes": Path(str(base) + "ThreadScopes.csv"),
        "timing_events_dir": Path(str(base) + "TimingEvents"),
    }


def resolve_unreal_insights_path(explicit_path: Any, engine_root: Any) -> Path | None:
    direct = _resolve_existing_file(explicit_path)
    if direct:
        return direct

    candidate_roots: list[Path] = []
    explicit_root = _resolve_existing_dir(engine_root)
    if explicit_root:
        candidate_roots.append(explicit_root)

    for env_key in ("UE_ENGINE_ROOT", "UNREAL_ENGINE_ROOT", "UE5_ROOT", "ENGINE_ROOT"):
        from_env = _resolve_existing_dir(os.environ.get(env_key))
        if from_env:
            candidate_roots.append(from_env)

    candidate_roots.extend(path for path in (Path("D:/UE_5.5"), Path("D:/UE5")) if path.exists())

    for root in candidate_roots:
        insights = root / "Engine" / "Binaries" / "Win64" / "UnrealInsights.exe"
        if insights.exists():
            return insights
    return None


def _resolve_existing_file(raw_path: Any) -> Path | None:
    if isinstance(raw_path, str) and raw_path.strip():
        path = Path(raw_path.strip())
        if path.exists() and path.is_file():
            return path
    return None


def _resolve_existing_dir(raw_path: Any) -> Path | None:
    if isinstance(raw_path, str) and raw_path.strip():
        path = Path(raw_path.strip())
        if path.exists() and path.is_dir():
            return path
    return None


def _safe_float(value: Any) -> float | None:
    if value in (None, "", "nan", "-nan(ind)", "nan(ind)"):
        return None
    try:
        parsed = float(value)
    except (TypeError, ValueError):
        return None
    if math.isnan(parsed) or math.isinf(parsed):
        return None
    return parsed


@dataclass
class _RunningStats:
    count: int = 0
    total: float = 0.0
    minimum: float = math.inf
    maximum: float = 0.0
    mean: float = 0.0
    m2: float = 0.0
    first_start: float | None = None
    first_finish: float | None = None
    first_duration: float | None = None
    last_start: float | None = None
    last_finish: float | None = None
    last_duration: float | None = None

    def add(self, start: float, finish: float) -> None:
        duration = finish - start
        if duration < 0:
            return

        self.count += 1
        self.total += duration
        self.minimum = min(self.minimum, duration)
        self.maximum = max(self.maximum, duration)

        if self.first_start is None:
            self.first_start = start
            self.first_finish = finish
            self.first_duration = duration
        self.last_start = start
        self.last_finish = finish
        self.last_duration = duration

        delta = duration - self.mean
        self.mean += delta / self.count
        delta2 = duration - self.mean
        self.m2 += delta * delta2

    @property
    def deviation(self) -> float:
        if self.count <= 1:
            return 0.0
        return math.sqrt(self.m2 / (self.count - 1))


@dataclass
class _ScopeAgg:
    thread_id: int
    thread_name: str
    name: str
    stats: _RunningStats = field(default_factory=_RunningStats)


def _write_response_file(path: Path, lines: list[str]) -> None:
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def _run_unreal_insights_export(
    *,
    trace_path: Path,
    insights_path: Path,
    response_file: Path,
    timeout_seconds: int,
) -> subprocess.CompletedProcess[str]:
    log_path = Path(str(trace_path.with_suffix("")) + f"_insights_{response_file.stem}.log")
    command = [
        str(insights_path),
        f"-OpenTraceFile={trace_path}",
        "-AutoQuit",
        "-NoUI",
        f"-ExecOnAnalysisCompleteCmd=@={response_file}",
        f"-ABSLOG={log_path}",
        "-log",
    ]
    return subprocess.run(
        command,
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
        timeout=timeout_seconds,
        check=False,
    )


def export_threads_csv(
    trace_path: Path,
    *,
    insights_path: Path,
    timeout_seconds: int = _DEFAULT_INSIGHTS_TIMEOUT_SECONDS,
) -> Path:
    paths = csv_paths_from_trace(trace_path)
    output_csv = paths["threads"]
    if output_csv.exists() and output_csv.stat().st_mtime >= trace_path.stat().st_mtime:
        return output_csv

    rsp_path = Path(str(trace_path.with_suffix("")) + f"_{trace_path.stem}_export_threads.rsp")
    _write_response_file(rsp_path, [f"TimingInsights.ExportThreads {output_csv.as_posix()}"])
    _run_unreal_insights_export(
        trace_path=trace_path,
        insights_path=insights_path,
        response_file=rsp_path,
        timeout_seconds=timeout_seconds,
    )
    if not output_csv.exists():
        raise FileNotFoundError(f"Threads.csv was not generated: {output_csv}")
    return output_csv


def _timing_events_csv_path(trace_path: Path, thread_name: str) -> tuple[Path, Path | None]:
    paths = csv_paths_from_trace(trace_path)
    safe_name = thread_name.replace(" ", "_").replace("/", "_")
    preferred = paths["timing_events_dir"] / f"{trace_path.stem}_{safe_name}.csv"
    legacy = trace_path.with_name(f"{trace_path.stem}TimingEvents_{safe_name}.csv")
    if preferred.exists():
        return preferred, None
    if legacy.exists():
        return legacy, preferred
    return preferred, None


def _timing_events_csv_has_data(path: Path) -> bool:
    if not path.exists() or path.stat().st_size <= 64:
        return False
    with path.open("r", encoding="utf-8-sig", newline="") as handle:
        reader = csv.reader(handle)
        next(reader, None)
        return next(reader, None) is not None


def export_timing_events_csv(
    trace_path: Path,
    *,
    insights_path: Path,
    thread_name: str,
    output_csv: Path,
    time_start_seconds: float | None = None,
    time_end_seconds: float | None = None,
    timeout_seconds: int = _DEFAULT_INSIGHTS_TIMEOUT_SECONDS,
) -> Path:
    if (
        output_csv.exists()
        and output_csv.stat().st_mtime >= trace_path.stat().st_mtime
        and _timing_events_csv_has_data(output_csv)
    ):
        return output_csv

    preferred_csv, legacy_csv = _timing_events_csv_path(trace_path, thread_name)
    if (
        preferred_csv != output_csv
        and preferred_csv.exists()
        and preferred_csv.stat().st_mtime >= trace_path.stat().st_mtime
        and _timing_events_csv_has_data(preferred_csv)
    ):
        return preferred_csv
    if (
        legacy_csv is not None
        and legacy_csv.exists()
        and legacy_csv.stat().st_mtime >= trace_path.stat().st_mtime
        and _timing_events_csv_has_data(legacy_csv)
    ):
        return legacy_csv

    output_csv.parent.mkdir(parents=True, exist_ok=True)
    safe_thread = thread_name.replace(" ", "_").replace("/", "_")
    rsp_path = Path(str(trace_path.with_suffix("")) + f"_{trace_path.stem}_export_timing_{safe_thread}.rsp")

    command = (
        f"TimingInsights.ExportTimingEvents {output_csv.as_posix()} "
        f"-columns=ThreadId,ThreadName,TimerName,StartTime,EndTime,Duration "
        f'-threads="{thread_name}" -timers=*'
    )
    if time_start_seconds is not None:
        command += f" -startTime={time_start_seconds}"
    if time_end_seconds is not None:
        command += f" -endTime={time_end_seconds}"

    _write_response_file(rsp_path, [command])
    _run_unreal_insights_export(
        trace_path=trace_path,
        insights_path=insights_path,
        response_file=rsp_path,
        timeout_seconds=timeout_seconds,
    )
    if not output_csv.exists():
        raise FileNotFoundError(f"TimingEvents export was not generated: {output_csv}")
    return output_csv


def aggregate_timing_events_csv(
    events_csv: Path,
    *,
    thread_scopes_csv: Path,
    append: bool = False,
) -> int:
    aggregates: dict[tuple[int, str, str], _ScopeAgg] = {}

    with events_csv.open("r", encoding="utf-8-sig", newline="") as handle:
        reader = csv.DictReader(handle)
        for row in reader:
            thread_id_raw = row.get("ThreadId")
            thread_name = (row.get("ThreadName") or "").strip()
            timer_name = (row.get("TimerName") or row.get("Name") or "").strip()
            start = _safe_float(row.get("StartTime"))
            end = _safe_float(row.get("EndTime"))
            duration = _safe_float(row.get("Duration"))
            if not timer_name or thread_id_raw is None:
                continue
            try:
                thread_id = int(thread_id_raw)
            except (TypeError, ValueError):
                continue

            if start is None and end is not None and duration is not None:
                start = end - duration
            if end is None and start is not None and duration is not None:
                end = start + duration
            if start is None or end is None:
                continue

            key = (thread_id, thread_name, timer_name)
            agg = aggregates.get(key)
            if agg is None:
                agg = _ScopeAgg(thread_id=thread_id, thread_name=thread_name, name=timer_name)
                aggregates[key] = agg
            agg.stats.add(start, end)

    if not aggregates:
        return 0

    thread_scopes_csv.parent.mkdir(parents=True, exist_ok=True)
    write_header = not append or not thread_scopes_csv.exists()
    mode = "a" if append and thread_scopes_csv.exists() else "w"
    with thread_scopes_csv.open(mode, encoding="utf-8", newline="") as handle:
        writer = csv.writer(handle)
        if write_header:
            writer.writerow(
                [
                    "ThreadId",
                    "ThreadName",
                    "Name",
                    "Count",
                    "TotalDurationSeconds",
                    "FirstStartSeconds",
                    "FirstFinishSeconds",
                    "FirstDurationSeconds",
                    "LastStartSeconds",
                    "LastFinishSeconds",
                    "LastDurationSeconds",
                    "MinDurationSeconds",
                    "MaxDurationSeconds",
                    "MeanDurationSeconds",
                    "DeviationDurationSeconds",
                ]
            )

        rows = sorted(aggregates.values(), key=lambda item: item.stats.total, reverse=True)
        for item in rows:
            stats = item.stats
            writer.writerow(
                [
                    item.thread_id,
                    item.thread_name,
                    item.name,
                    stats.count,
                    stats.total,
                    stats.first_start,
                    stats.first_finish,
                    stats.first_duration,
                    stats.last_start,
                    stats.last_finish,
                    stats.last_duration,
                    0.0 if stats.minimum is math.inf else stats.minimum,
                    stats.maximum,
                    stats.mean,
                    stats.deviation,
                ]
            )
    return len(aggregates)


def build_thread_scopes_summary(
    trace_path: Path,
    *,
    insights_path: Path,
    thread_names: list[str] | None = None,
    time_start_seconds: float | None = None,
    time_end_seconds: float | None = None,
    timeout_seconds: int = _DEFAULT_INSIGHTS_TIMEOUT_SECONDS,
) -> dict[str, Any]:
    paths = csv_paths_from_trace(trace_path)
    threads_csv = export_threads_csv(
        trace_path,
        insights_path=insights_path,
        timeout_seconds=timeout_seconds,
    )

    selected_threads = thread_names or list(_DEFAULT_EXPORT_THREADS)
    thread_scopes_csv = paths["thread_scopes"]
    timing_dir = paths["timing_events_dir"]
    timing_dir.mkdir(parents=True, exist_ok=True)

    if thread_scopes_csv.exists():
        thread_scopes_csv.unlink()

    exported_files: list[str] = []
    aggregated_rows = 0
    for index, thread_name in enumerate(selected_threads):
        preferred_csv, _legacy_csv = _timing_events_csv_path(trace_path, thread_name)
        events_csv = preferred_csv
        export_timing_events_csv(
            trace_path,
            insights_path=insights_path,
            thread_name=thread_name,
            output_csv=events_csv,
            time_start_seconds=time_start_seconds,
            time_end_seconds=time_end_seconds,
            timeout_seconds=timeout_seconds,
        )
        exported_files.append(str(events_csv))
        aggregated_rows += aggregate_timing_events_csv(
            events_csv,
            thread_scopes_csv=thread_scopes_csv,
            append=index > 0,
        )

    if not thread_scopes_csv.exists():
        raise FileNotFoundError(f"ThreadScopes.csv was not generated: {thread_scopes_csv}")

    return {
        "threadsCsv": str(threads_csv),
        "threadScopesCsv": str(thread_scopes_csv),
        "timingEventExports": exported_files,
        "aggregatedRows": aggregated_rows,
        "exportedThreads": selected_threads,
    }


def read_csv_rows(path: Path) -> list[dict[str, str]]:
    with path.open("r", encoding="utf-8-sig", newline="") as handle:
        return list(csv.DictReader(handle))


def parse_thread_scope_row(row: dict[str, str]) -> dict[str, Any]:
    return {
        "thread_id": int(row.get("ThreadId", "0") or 0),
        "thread_name": row.get("ThreadName", ""),
        "name": row.get("Name", ""),
        "count": int(row.get("Count", "0") or 0),
        "total": _safe_float(row.get("TotalDurationSeconds")),
        "first_start": _safe_float(row.get("FirstStartSeconds")),
        "first_finish": _safe_float(row.get("FirstFinishSeconds")),
        "last_start": _safe_float(row.get("LastStartSeconds")),
        "last_finish": _safe_float(row.get("LastFinishSeconds")),
        "min": _safe_float(row.get("MinDurationSeconds")),
        "max": _safe_float(row.get("MaxDurationSeconds")),
        "mean": _safe_float(row.get("MeanDurationSeconds")),
    }


def load_thread_scope_data(trace_path: Path) -> dict[str, Any]:
    paths = csv_paths_from_trace(trace_path)
    thread_scopes_path = paths["thread_scopes"]
    threads_path = paths["threads"]
    if not thread_scopes_path.exists():
        raise FileNotFoundError(f"Missing ThreadScopes.csv for {trace_path.name}")

    thread_scopes = [parse_thread_scope_row(row) for row in read_csv_rows(thread_scopes_path)]
    threads = read_csv_rows(threads_path) if threads_path.exists() else []
    return {
        "thread_scopes": thread_scopes,
        "threads": threads,
        "csv_paths": {
            "thread_scopes": str(thread_scopes_path),
            "threads": str(threads_path) if threads_path.exists() else None,
        },
    }


def summarize_threads(thread_scopes: list[dict[str, Any]], *, limit: int = 12) -> list[dict[str, Any]]:
    totals: dict[str, dict[str, Any]] = {}
    for row in thread_scopes:
        thread_name = row.get("thread_name") or f"Thread_{row.get('thread_id')}"
        entry = totals.setdefault(
            thread_name,
            {
                "thread_name": thread_name,
                "thread_id": row.get("thread_id"),
                "scope_count": 0,
                "total": 0.0,
                "max": 0.0,
            },
        )
        entry["scope_count"] += 1
        entry["total"] += row.get("total") or 0.0
        entry["max"] = max(entry["max"], row.get("max") or 0.0)

    rows = list(totals.values())
    rows.sort(key=lambda item: item["total"], reverse=True)
    return rows[:limit]


def top_scopes_by_thread(
    thread_scopes: list[dict[str, Any]],
    *,
    metric: str = "total",
    per_thread_limit: int = 8,
    thread_names: list[str] | None = None,
) -> dict[str, list[dict[str, Any]]]:
    selected = set(thread_names or [])
    grouped: dict[str, list[dict[str, Any]]] = {}
    for row in thread_scopes:
        thread_name = row.get("thread_name") or f"Thread_{row.get('thread_id')}"
        if selected and thread_name not in selected:
            continue
        grouped.setdefault(thread_name, []).append(row)

    result: dict[str, list[dict[str, Any]]] = {}
    for thread_name, rows in grouped.items():
        rows.sort(key=lambda item: item.get(metric) or 0.0, reverse=True)
        result[thread_name] = rows[:per_thread_limit]
    return result


def window_thread_scopes(
    thread_scopes: list[dict[str, Any]],
    time_start: float,
    time_end: float,
    *,
    limit: int = 20,
) -> list[dict[str, Any]]:
    result: list[dict[str, Any]] = []
    for row in thread_scopes:
        start = row.get("first_start")
        finish = row.get("last_finish") or row.get("first_finish")
        if start is None or finish is None:
            continue
        if finish < time_start or start > time_end:
            continue
        result.append(row)
    result.sort(key=lambda item: ((item.get("max") or 0.0), (item.get("total") or 0.0)), reverse=True)
    return result[:limit]


def attribute_scope_ownership(
    thread_scopes: list[dict[str, Any]],
    scope_names: list[str],
) -> list[dict[str, Any]]:
    lowered = [name.lower() for name in scope_names]
    best: dict[str, dict[str, Any]] = {}
    for row in thread_scopes:
        name = row.get("name") or ""
        if not any(needle in name.lower() for needle in lowered):
            continue
        existing = best.get(name)
        if existing is None or (row.get("total") or 0.0) > (existing.get("total") or 0.0):
            best[name] = {
                "name": name,
                "thread_name": row.get("thread_name"),
                "thread_id": row.get("thread_id"),
                "total": row.get("total"),
                "max": row.get("max"),
                "mean": row.get("mean"),
                "count": row.get("count"),
            }
    rows = list(best.values())
    rows.sort(key=lambda item: item.get("total") or 0.0, reverse=True)
    return rows
