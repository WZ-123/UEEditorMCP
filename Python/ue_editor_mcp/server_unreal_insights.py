from __future__ import annotations

import asyncio
import csv
import json
import logging
import math
import os
import re
import subprocess
from pathlib import Path
from typing import Any

from ue_editor_mcp import utrace_thread_analysis as thread_analysis

from mcp.server import Server
from mcp.server.stdio import stdio_server
from mcp.types import TextContent, Tool

logger = logging.getLogger(__name__)

_FRAME_RE = re.compile(r"^Frame\s+(\d+)$")
_DEFAULT_TIMEOUT_SECONDS = 300
_MAX_OUTPUT_CHARS = 16000
_DEFAULT_THREAD_EXPORT_TIMEOUT_SECONDS = 900

_KNOWN_EDITOR_ENV_KEYS = [
    "UE_EDITOR_CMD",
    "UNREAL_EDITOR_CMD",
]

_KNOWN_ENGINE_ROOT_ENV_KEYS = [
    "UE_ENGINE_ROOT",
    "UNREAL_ENGINE_ROOT",
    "UE5_ROOT",
    "ENGINE_ROOT",
]

_KNOWN_PROJECT_ENV_KEYS = [
    "UPROJECT_PATH",
    "PROJECT_FILE",
]


def _to_text(result: dict[str, Any]) -> list[TextContent]:
    return [TextContent(type="text", text=json.dumps(result, ensure_ascii=False, indent=2))]


def _clamp_int(value: Any, default_value: int, min_value: int, max_value: int) -> int:
    try:
        parsed = int(value)
    except (TypeError, ValueError):
        parsed = default_value
    return max(min_value, min(max_value, parsed))


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


def _infer_workspace_root() -> Path | None:
    env_keys = [
        "UE_MCP_WORKSPACE_ROOT",
        "WORKSPACE_ROOT",
        "VSCODE_WORKSPACE_FOLDER",
        "PROJECT_ROOT",
    ]
    for key in env_keys:
        value = os.environ.get(key)
        if value:
            candidate = Path(value)
            if candidate.exists():
                return candidate

    try:
        candidate = Path(__file__).resolve().parents[4]
        if candidate.exists():
            return candidate
    except Exception:
        return None

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


def _resolve_project_path(explicit_project_path: Any) -> Path | None:
    direct = _resolve_existing_file(explicit_project_path)
    if direct and direct.suffix.lower() == ".uproject":
        return direct

    for key in _KNOWN_PROJECT_ENV_KEYS:
        from_env = _resolve_existing_file(os.environ.get(key))
        if from_env and from_env.suffix.lower() == ".uproject":
            return from_env

    workspace_root = _infer_workspace_root()
    if not workspace_root:
        return None

    candidates = sorted(workspace_root.glob("*.uproject"))
    if len(candidates) == 1:
        return candidates[0]
    return candidates[0] if candidates else None


def _resolve_editor_cmd_path(explicit_editor_cmd: Any, explicit_engine_root: Any) -> Path | None:
    direct = _resolve_existing_file(explicit_editor_cmd)
    if direct:
        return direct

    candidate_roots: list[Path] = []

    explicit_root = _resolve_existing_dir(explicit_engine_root)
    if explicit_root:
        candidate_roots.append(explicit_root)

    for key in _KNOWN_EDITOR_ENV_KEYS:
        from_env_cmd = _resolve_existing_file(os.environ.get(key))
        if from_env_cmd:
            return from_env_cmd

    for key in _KNOWN_ENGINE_ROOT_ENV_KEYS:
        from_env_root = _resolve_existing_dir(os.environ.get(key))
        if from_env_root:
            candidate_roots.append(from_env_root)

    common_roots = [
        Path("D:/UE_5.5"),
        Path("D:/UE5"),
        Path("C:/Program Files/Epic Games/UE_5.5"),
        Path("C:/Program Files/Epic Games/UE_5.4"),
        Path("C:/Program Files/Epic Games/UE_5.3"),
    ]
    candidate_roots.extend(root for root in common_roots if root.exists())

    for root in candidate_roots:
        cmd = root / "Engine" / "Binaries" / "Win64" / "UnrealEditor-Cmd.exe"
        if cmd.exists():
            return cmd

    return None


def _csv_paths_from_trace(trace_path: Path) -> dict[str, Path]:
    base = trace_path.with_suffix("")
    thread_paths = thread_analysis.csv_paths_from_trace(trace_path)
    return {
        "scopes": Path(str(base) + "Scopes.csv"),
        "bookmarks": Path(str(base) + "Bookmarks.csv"),
        "counters": Path(str(base) + "Counters.csv"),
        "telemetry": Path(str(base) + "Telemetry.csv"),
        "threads": thread_paths["threads"],
        "thread_scopes": thread_paths["thread_scopes"],
    }


def _read_csv_rows(path: Path) -> list[dict[str, str]]:
    with path.open("r", encoding="utf-8-sig", newline="") as handle:
        reader = csv.DictReader(handle)
        return [dict(row) for row in reader]


def _summarize_output(output: str) -> str:
    output = output.strip()
    if len(output) <= _MAX_OUTPUT_CHARS:
        return output
    return output[-_MAX_OUTPUT_CHARS:]


def _parse_scope_row(row: dict[str, str]) -> dict[str, Any]:
    return {
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


def _parse_bookmark_row(row: dict[str, str]) -> dict[str, Any]:
    return {
        "name": row.get("Name", ""),
        "first": _safe_float(row.get("FirstSeconds")),
        "last": _safe_float(row.get("LastSeconds")),
        "count": int(row.get("Count", "0") or 0),
    }


def _resolve_insights_path(explicit_insights_path: Any, explicit_engine_root: Any) -> Path | None:
    return thread_analysis.resolve_unreal_insights_path(explicit_insights_path, explicit_engine_root)


def _known_scope_thread_attribution(scopes: list[dict[str, Any]], thread_scopes: list[dict[str, Any]]) -> list[dict[str, Any]]:
    needles = [
        "ATerrainChunk::UploadMesh",
        "GenQueue_Physics",
        "ATerrainChunk::PrepareMesh",
        "FillNoise_Task",
        "FlushLevelStreaming",
        "UpdateLevelStreaming",
        "FDeferredShadingSceneRenderer_Render",
        "ShadowDepths",
        "WaitForTasks",
        "Slate::Tick",
    ]
    return thread_analysis.attribute_scope_ownership(thread_scopes, needles)


def _load_trace_data(trace_path: Path, *, include_thread_scopes: bool = False) -> dict[str, Any]:
    csv_paths = _csv_paths_from_trace(trace_path)
    missing = [name for name, path in csv_paths.items() if name in {"scopes", "bookmarks"} and not path.exists()]
    if missing:
        raise FileNotFoundError(f"Missing CSV summaries for {trace_path.name}: {', '.join(missing)}")

    scopes = [_parse_scope_row(row) for row in _read_csv_rows(csv_paths["scopes"])]
    bookmarks = [_parse_bookmark_row(row) for row in _read_csv_rows(csv_paths["bookmarks"])]

    result: dict[str, Any] = {
        "trace_path": trace_path,
        "csv_paths": {name: str(path) for name, path in csv_paths.items() if path.exists()},
        "scopes": scopes,
        "bookmarks": bookmarks,
    }

    if include_thread_scopes and csv_paths["thread_scopes"].exists():
        thread_loaded = thread_analysis.load_thread_scope_data(trace_path)
        result["thread_scopes"] = thread_loaded["thread_scopes"]
        result["threads"] = thread_loaded["threads"]
        if thread_loaded["csv_paths"].get("threads"):
            result["csv_paths"]["threads"] = thread_loaded["csv_paths"]["threads"]
        result["csv_paths"]["thread_scopes"] = thread_loaded["csv_paths"]["thread_scopes"]

    return result


def _top_scopes(scopes: list[dict[str, Any]], *, metric: str, limit: int, include_frames: bool = False) -> list[dict[str, Any]]:
    rows = []
    for row in scopes:
        if not include_frames and _FRAME_RE.match(row["name"]):
            continue
        value = row.get(metric)
        if value is None:
            continue
        rows.append({
            "name": row["name"],
            "count": row["count"],
            "total": row["total"],
            "max": row["max"],
            "mean": row["mean"],
            "value": value,
        })
    rows.sort(key=lambda item: item["value"], reverse=True)
    return rows[:limit]


def _match_scope(scopes: list[dict[str, Any]], *needles: str) -> list[dict[str, Any]]:
    lowered = [needle.lower() for needle in needles]
    return [row for row in scopes if any(needle in row["name"].lower() for needle in lowered)]


def _bookmark_prefix(name: str) -> str:
    if " - " in name:
        return name.split(" - ", 1)[0]
    return name


def _bookmark_clusters(bookmarks: list[dict[str, Any]]) -> list[dict[str, Any]]:
    grouped: dict[str, dict[str, Any]] = {}
    for row in bookmarks:
        prefix = _bookmark_prefix(row["name"])
        entry = grouped.setdefault(prefix, {"name": prefix, "count": 0, "first": None, "last": None})
        entry["count"] += row["count"]
        first = row["first"]
        last = row["last"]
        if first is not None:
            entry["first"] = first if entry["first"] is None else min(entry["first"], first)
        if last is not None:
            entry["last"] = last if entry["last"] is None else max(entry["last"], last)
    result = list(grouped.values())
    result.sort(key=lambda item: (item["count"], item["first"] or -1), reverse=True)
    return result


def _frame_rows(scopes: list[dict[str, Any]]) -> list[dict[str, Any]]:
    rows: list[dict[str, Any]] = []
    for row in scopes:
        match = _FRAME_RE.match(row["name"])
        if not match:
            continue
        frame_number = int(match.group(1))
        start = row["first_start"]
        finish = row["last_finish"] or row["first_finish"]
        duration = row["total"]
        if start is None or finish is None:
            continue
        rows.append({
            "frame": frame_number,
            "start": start,
            "finish": finish,
            "duration": duration,
        })
    rows.sort(key=lambda item: item["frame"])
    return rows


def _estimate_time_window(
    frames: list[dict[str, Any]],
    frame_start: int | None,
    frame_end: int | None,
    time_start: float | None,
    time_end: float | None,
) -> tuple[float | None, float | None, dict[str, Any]]:
    note: dict[str, Any] = {"mapping": "direct_time"}
    if time_start is not None and time_end is not None:
        return time_start, time_end, note

    if frame_start is None or frame_end is None or not frames:
        return None, None, {"mapping": "unavailable"}

    in_range = [row for row in frames if frame_start <= row["frame"] <= frame_end]
    if in_range:
        return (
            min(row["start"] for row in in_range),
            max(row["finish"] for row in in_range),
            {"mapping": "frame_rows", "frames_found": len(in_range)},
        )

    all_numbers = [row["frame"] for row in frames]
    min_frame = min(all_numbers)
    max_frame = max(all_numbers)
    if max_frame == min_frame:
        return None, None, {"mapping": "unavailable"}

    first_row = frames[0]
    last_row = frames[-1]
    total_span = last_row["finish"] - first_row["start"]
    if total_span <= 0:
        return None, None, {"mapping": "unavailable"}

    normalized_start = (frame_start - min_frame) / (max_frame - min_frame)
    normalized_end = (frame_end - min_frame) / (max_frame - min_frame)
    estimated_start = first_row["start"] + total_span * normalized_start
    estimated_end = first_row["start"] + total_span * normalized_end
    return (
        estimated_start,
        estimated_end,
        {"mapping": "interpolated", "frame_min": min_frame, "frame_max": max_frame},
    )


def _window_bookmarks(bookmarks: list[dict[str, Any]], time_start: float, time_end: float) -> list[dict[str, Any]]:
    result = []
    for row in bookmarks:
        first = row["first"]
        if first is None:
            continue
        if time_start <= first <= time_end:
            result.append(row)
    return result


def _window_scopes(scopes: list[dict[str, Any]], time_start: float, time_end: float) -> list[dict[str, Any]]:
    result = []
    window_size = max(time_end - time_start, 0.001)
    max_span = max(5.0, window_size * 10.0)
    for row in scopes:
        if _FRAME_RE.match(row["name"]):
            continue
        start = row["first_start"]
        finish = row["last_finish"] or row["first_finish"]
        if start is None or finish is None:
            continue
        span = finish - start
        if span < 0:
            continue
        if span > max_span:
            continue
        if finish < time_start or start > time_end:
            continue
        result.append(row)
    result.sort(key=lambda item: ((item["max"] or 0.0), (item["total"] or 0.0)), reverse=True)
    return result


def _pattern_detection(scopes: list[dict[str, Any]], bookmarks: list[dict[str, Any]]) -> list[dict[str, Any]]:
    detections: list[dict[str, Any]] = []

    terrain = _match_scope(
        scopes,
        "ATerrainChunk::UploadMesh",
        "GenQueue_Physics",
        "ATerrainChunk::PrepareMesh",
        "FillNoise_Task",
        "UNoiseGenerator::FillChunkData",
    )
    terrain_max = max((row["max"] or 0.0) for row in terrain) if terrain else 0.0
    if terrain and terrain_max >= 0.02:
        detections.append({
            "pattern": "terrain_or_collision_spike",
            "confidence": "high" if terrain_max >= 0.05 else "medium",
            "evidence": [row["name"] for row in terrain[:5]],
            "why": "Terrain preparation, upload, or near-player physics interaction has visible CPU cost.",
        })

    streaming = _match_scope(
        scopes,
        "FlushLevelStreaming",
        "UpdateLevelStreaming",
        "LevelStreamingAlwaysLoaded",
        "RequestLevel",
        "LoadMap",
    )
    bookmark_prefixes = {_bookmark_prefix(row["name"]) for row in bookmarks}
    if streaming or {"RequestLevel", "RequestLevelComplete", "LoadMap", "LoadMapComplete"} & bookmark_prefixes:
        detections.append({
            "pattern": "streaming_completion_burst",
            "confidence": "high" if streaming else "medium",
            "evidence": [row["name"] for row in streaming[:5]] or sorted(bookmark_prefixes),
            "why": "Level streaming, level instance completion, or map load work is clustered into a short span.",
        })

    render = _match_scope(
        scopes,
        "FDeferredShadingSceneRenderer_Render",
        "ShadowDepths",
        "Nanite::DrawGeometry",
        "LumenScreenProbeGather",
        "RHI_SubmitToGPU",
        "D3D12_Present",
    )
    render_total = sum((row["total"] or 0.0) for row in render)
    if render and render_total >= 1.0:
        detections.append({
            "pattern": "steady_rendering_pressure",
            "confidence": "medium",
            "evidence": [row["name"] for row in render[:6]],
            "why": "Rendering-related scopes account for a large and relatively steady share of time.",
        })

    gc = _match_scope(scopes, "CollectGarbage", "FRealtimeGC", "ConditionalCollectGarbage")
    if gc:
        gc_max = max((row["max"] or 0.0) for row in gc)
        if gc_max >= 0.02:
            detections.append({
                "pattern": "gc_event",
                "confidence": "medium",
                "evidence": [row["name"] for row in gc[:5]],
                "why": "Garbage collection scopes show measurable spikes.",
            })

    return detections


def _summarize_trace_data(
    trace_path: Path,
    scopes: list[dict[str, Any]],
    bookmarks: list[dict[str, Any]],
    thread_scopes: list[dict[str, Any]] | None = None,
) -> dict[str, Any]:
    summary = {
        "success": True,
        "tracePath": str(trace_path),
        "topByTotalDuration": _top_scopes(scopes, metric="total", limit=12),
        "topByMaxDuration": _top_scopes(scopes, metric="max", limit=12),
        "bookmarkClusters": _bookmark_clusters(bookmarks)[:12],
        "patternDetections": _pattern_detection(scopes, bookmarks),
        "notes": [
            "Scopes.csv is aggregated across all threads. Use threadScopes/topScopesByThread for per-thread ownership.",
            "Frame-to-time mapping derived from Scopes.csv is approximate unless the requested frame rows are present directly.",
        ],
    }

    if thread_scopes:
        summary["threadTotals"] = thread_analysis.summarize_threads(thread_scopes, limit=12)
        summary["topScopesByThread"] = thread_analysis.top_scopes_by_thread(
            thread_scopes,
            metric="total",
            per_thread_limit=8,
        )
        summary["scopeThreadOwnership"] = _known_scope_thread_attribution(scopes, thread_scopes)
        summary["notes"].insert(
            0,
            "ThreadScopes.csv provides per-thread scope ownership for the exported thread set.",
        )
    else:
        summary["notes"].insert(
            0,
            "ThreadScopes.csv was not found. Run unreal_insights_summarize_thread_scopes before analysis for per-thread attribution.",
        )

    return summary


def _format_explanation(summary: dict[str, Any]) -> dict[str, Any]:
    detections = summary.get("patternDetections", [])
    top_detection = detections[0] if detections else None
    if not top_detection:
        primary = "No strong single pattern was detected from CSV-only analysis."
    else:
        mapping = {
            "terrain_or_collision_spike": "Most likely a terrain preparation or collision-related spike.",
            "streaming_completion_burst": "Most likely a level streaming completion burst on the game thread.",
            "steady_rendering_pressure": "Most likely a steady rendering cost issue rather than one blocking CPU spike.",
            "gc_event": "Most likely a garbage collection related hitch.",
        }
        primary = mapping.get(top_detection["pattern"], top_detection["why"])

    return {
        "success": True,
        "primaryConclusion": primary,
        "supportingPatterns": detections,
        "topByMaxDuration": summary.get("topByMaxDuration", [])[:6],
        "bookmarkClusters": summary.get("bookmarkClusters", [])[:6],
        "notes": summary.get("notes", []),
    }


def _handle_summarize_trace(args: dict[str, Any]) -> dict[str, Any]:
    trace_path = _resolve_existing_file(args.get("tracePath"))
    if not trace_path:
        return {
            "success": False,
            "error": "Missing or invalid tracePath",
            "notes": ["Please pass a valid .utrace file path."],
        }

    project_path = _resolve_project_path(args.get("projectPath"))
    if not project_path:
        return {
            "success": False,
            "error": "Could not resolve .uproject path",
            "notes": ["Pass projectPath explicitly or set UPROJECT_PATH/PROJECT_ROOT/WORKSPACE_ROOT."],
        }

    editor_cmd = _resolve_editor_cmd_path(args.get("editorCmdPath"), args.get("engineRoot"))
    if not editor_cmd:
        return {
            "success": False,
            "error": "Could not resolve UnrealEditor-Cmd.exe",
            "notes": ["Pass editorCmdPath explicitly or set UE_EDITOR_CMD / UE_ENGINE_ROOT."],
        }

    timeout_seconds = _clamp_int(args.get("timeoutSeconds"), _DEFAULT_TIMEOUT_SECONDS, 10, 3600)
    include_telemetry = bool(args.get("includeTelemetry", True))
    skip_baseline = bool(args.get("skipBaseline", True))

    command = [
        str(editor_cmd),
        str(project_path),
        "-run=SummarizeTrace",
        f"-inputfile={trace_path}",
        "-unattended",
        "-nop4",
        "-nosplash",
        "-nullrhi",
    ]
    if include_telemetry:
        command.append("-alltelemetry")
    if skip_baseline:
        command.append("-skipbaseline")

    extra_args = args.get("extraArgs")
    if isinstance(extra_args, list):
        for item in extra_args:
            if isinstance(item, str) and item.strip():
                command.append(item.strip())

    try:
        completed = subprocess.run(
            command,
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="replace",
            timeout=timeout_seconds,
            check=False,
        )
    except subprocess.TimeoutExpired as exc:
        return {
            "success": False,
            "error": f"SummarizeTrace timed out after {timeout_seconds}s",
            "notes": ["Increase timeoutSeconds for very large trace files."],
            "partialOutput": _summarize_output((exc.stdout or "") + "\n" + (exc.stderr or "")),
        }
    except Exception as exc:
        return {
            "success": False,
            "error": str(exc),
            "notes": ["Failed to launch UnrealEditor-Cmd.exe."],
        }

    csv_paths = _csv_paths_from_trace(trace_path)
    generated = {name: str(path) for name, path in csv_paths.items() if path.exists()}
    output = _summarize_output((completed.stdout or "") + "\n" + (completed.stderr or ""))

    success = csv_paths["scopes"].exists() and csv_paths["bookmarks"].exists()
    notes: list[str] = []
    if completed.returncode != 0 and success:
        notes.append("Command returned a non-zero exit code, but required CSV outputs were generated.")
    elif completed.returncode != 0:
        notes.append("Command failed before producing the required CSV summaries.")

    thread_scope_result: dict[str, Any] | None = None
    if success and bool(args.get("includeThreadScopes", False)):
        insights_path = _resolve_insights_path(args.get("insightsPath"), args.get("engineRoot"))
        if not insights_path:
            notes.append("includeThreadScopes was requested but UnrealInsights.exe could not be resolved.")
        else:
            thread_timeout = _clamp_int(
                args.get("threadExportTimeoutSeconds"),
                _DEFAULT_THREAD_EXPORT_TIMEOUT_SECONDS,
                30,
                7200,
            )
            thread_names = args.get("threadNames")
            if isinstance(thread_names, list):
                thread_names = [str(name).strip() for name in thread_names if isinstance(name, str) and name.strip()]
            else:
                thread_names = None
            try:
                thread_scope_result = thread_analysis.build_thread_scopes_summary(
                    trace_path,
                    insights_path=insights_path,
                    thread_names=thread_names,
                    time_start_seconds=_safe_float(args.get("timeStartSeconds")),
                    time_end_seconds=_safe_float(args.get("timeEndSeconds")),
                    timeout_seconds=thread_timeout,
                )
                generated["threads"] = thread_scope_result["threadsCsv"]
                generated["thread_scopes"] = thread_scope_result["threadScopesCsv"]
            except subprocess.TimeoutExpired:
                notes.append(f"Thread scope export timed out after {thread_timeout}s.")
            except Exception as exc:
                notes.append(f"Thread scope export failed: {exc}")

    result: dict[str, Any] = {
        "success": success,
        "tracePath": str(trace_path),
        "projectPath": str(project_path),
        "editorCmdPath": str(editor_cmd),
        "exitCode": completed.returncode,
        "generatedCsvs": generated,
        "notes": notes,
        "outputTail": output,
    }
    if thread_scope_result:
        result["threadScopeExport"] = thread_scope_result
    return result


def _handle_analyze_trace(args: dict[str, Any]) -> dict[str, Any]:
    trace_path = _resolve_existing_file(args.get("tracePath"))
    if not trace_path:
        return {
            "success": False,
            "error": "Missing or invalid tracePath",
            "notes": ["This tool expects a tracePath pointing to the original .utrace file."],
        }

    try:
        loaded = _load_trace_data(trace_path, include_thread_scopes=True)
    except Exception as exc:
        return {"success": False, "error": str(exc)}

    summary = _summarize_trace_data(
        trace_path,
        loaded["scopes"],
        loaded["bookmarks"],
        loaded.get("thread_scopes"),
    )
    summary["csvPaths"] = loaded["csv_paths"]
    return summary


def _handle_analyze_frame_window(args: dict[str, Any]) -> dict[str, Any]:
    trace_path = _resolve_existing_file(args.get("tracePath"))
    if not trace_path:
        return {"success": False, "error": "Missing or invalid tracePath"}

    try:
        loaded = _load_trace_data(trace_path, include_thread_scopes=True)
    except Exception as exc:
        return {"success": False, "error": str(exc)}

    frame_start = args.get("frameStart")
    frame_end = args.get("frameEnd")
    time_start = _safe_float(args.get("timeStartSeconds"))
    time_end = _safe_float(args.get("timeEndSeconds"))

    if isinstance(frame_start, int) and frame_end is None:
        frame_end = frame_start
    if isinstance(frame_end, int) and frame_start is None:
        frame_start = frame_end

    frames = _frame_rows(loaded["scopes"])
    resolved_start, resolved_end, mapping = _estimate_time_window(frames, frame_start, frame_end, time_start, time_end)
    if resolved_start is None or resolved_end is None:
        return {
            "success": False,
            "error": "Could not resolve a time window from the provided arguments.",
            "notes": ["Provide frameStart/frameEnd with frame rows available in Scopes.csv, or pass explicit timeStartSeconds/timeEndSeconds."],
        }

    window_scopes = _window_scopes(loaded["scopes"], resolved_start, resolved_end)
    window_bookmarks = _window_bookmarks(loaded["bookmarks"], resolved_start, resolved_end)
    frame_samples = [row for row in frames if resolved_start <= row["start"] <= resolved_end or resolved_start <= row["finish"] <= resolved_end]

    pattern_summary = _pattern_detection(window_scopes, window_bookmarks)
    thread_window_scopes = []
    if loaded.get("thread_scopes"):
        thread_window_scopes = thread_analysis.window_thread_scopes(
            loaded["thread_scopes"],
            resolved_start,
            resolved_end,
            limit=20,
        )
    return {
        "success": True,
        "tracePath": str(trace_path),
        "requested": {
            "frameStart": frame_start,
            "frameEnd": frame_end,
            "timeStartSeconds": time_start,
            "timeEndSeconds": time_end,
        },
        "resolvedWindow": {
            "timeStartSeconds": resolved_start,
            "timeEndSeconds": resolved_end,
            "mapping": mapping,
        },
        "frameSamples": frame_samples[:64],
        "nearbyScopes": [
            {
                "name": row["name"],
                "count": row["count"],
                "total": row["total"],
                "max": row["max"],
                "first_start": row["first_start"],
                "last_finish": row["last_finish"],
            }
            for row in window_scopes[:20]
        ],
        "bookmarksInWindow": window_bookmarks[:20],
        "threadScopesInWindow": thread_window_scopes,
        "topScopesByThreadInWindow": thread_analysis.top_scopes_by_thread(
            thread_window_scopes,
            metric="max",
            per_thread_limit=6,
        ) if thread_window_scopes else {},
        "patternDetections": pattern_summary,
        "notes": [
            "Window analysis from CSV summaries is heuristic. It is strongest for streaming, GC, and one-off clustered work.",
            "threadScopesInWindow uses ThreadScopes.csv overlap filtering when available.",
        ],
    }


def _handle_summarize_thread_scopes(args: dict[str, Any]) -> dict[str, Any]:
    trace_path = _resolve_existing_file(args.get("tracePath"))
    if not trace_path:
        return {
            "success": False,
            "error": "Missing or invalid tracePath",
            "notes": ["Pass a valid .utrace file path."],
        }

    insights_path = _resolve_insights_path(args.get("insightsPath"), args.get("engineRoot"))
    if not insights_path:
        return {
            "success": False,
            "error": "Could not resolve UnrealInsights.exe",
            "notes": ["Pass insightsPath explicitly or set UE_ENGINE_ROOT."],
        }

    timeout_seconds = _clamp_int(args.get("timeoutSeconds"), _DEFAULT_THREAD_EXPORT_TIMEOUT_SECONDS, 30, 7200)
    thread_names = args.get("threadNames")
    if isinstance(thread_names, list):
        thread_names = [str(name).strip() for name in thread_names if isinstance(name, str) and name.strip()]
    else:
        thread_names = None

    time_start = _safe_float(args.get("timeStartSeconds"))
    time_end = _safe_float(args.get("timeEndSeconds"))

    try:
        result = thread_analysis.build_thread_scopes_summary(
            trace_path,
            insights_path=insights_path,
            thread_names=thread_names,
            time_start_seconds=time_start,
            time_end_seconds=time_end,
            timeout_seconds=timeout_seconds,
        )
    except subprocess.TimeoutExpired:
        return {
            "success": False,
            "error": f"UnrealInsights export timed out after {timeout_seconds}s",
            "notes": ["Increase timeoutSeconds or narrow threadNames/time window."],
        }
    except Exception as exc:
        return {"success": False, "error": str(exc)}

    return {
        "success": True,
        "tracePath": str(trace_path),
        "insightsPath": str(insights_path),
        **result,
        "notes": [
            "ThreadScopes.csv is aggregated from UnrealInsights timing-event exports.",
            "Default export threads: GameThread, RenderThread 0, RHIThread.",
        ],
    }


def _handle_explain_stutter_pattern(args: dict[str, Any]) -> dict[str, Any]:
    if any(key in args for key in ("frameStart", "frameEnd", "timeStartSeconds", "timeEndSeconds")):
        analysis = _handle_analyze_frame_window(args)
        if not analysis.get("success"):
            return analysis
        detections = analysis.get("patternDetections", [])
        if detections:
            top = detections[0]
            conclusion = top["why"]
        else:
            conclusion = "No dominant pattern was detected in the requested window from CSV-only evidence."
        return {
            "success": True,
            "mode": "frame_window",
            "primaryConclusion": conclusion,
            "resolvedWindow": analysis.get("resolvedWindow"),
            "patternDetections": detections,
            "bookmarksInWindow": analysis.get("bookmarksInWindow", [])[:10],
            "nearbyScopes": analysis.get("nearbyScopes", [])[:10],
            "notes": analysis.get("notes", []),
        }

    summary = _handle_analyze_trace(args)
    if not summary.get("success"):
        return summary
    explanation = _format_explanation(summary)
    explanation["mode"] = "whole_trace"
    return explanation


# Tool names must be [A-Za-z0-9_]+ only (Cursor MCP filters dotted names).
TOOLS = [
    Tool(
        name="unreal_insights_summarize_trace",
        description="Run UnrealEditor-Cmd SummarizeTrace for a .utrace file and return the generated CSV summary paths. This is the recommended first step before CSV-based analysis.",
        inputSchema={
            "type": "object",
            "properties": {
                "tracePath": {"type": "string", "description": "Absolute path to the .utrace file."},
                "projectPath": {"type": "string", "description": "Optional absolute path to the .uproject file. If omitted, the server will try to infer it."},
                "editorCmdPath": {"type": "string", "description": "Optional absolute path to UnrealEditor-Cmd.exe."},
                "engineRoot": {"type": "string", "description": "Optional Unreal Engine root directory used to resolve UnrealEditor-Cmd.exe."},
                "timeoutSeconds": {"type": "integer", "description": "Timeout for the commandlet process (default 300)."},
                "includeTelemetry": {"type": "boolean", "description": "Include -alltelemetry (default true)."},
                "skipBaseline": {"type": "boolean", "description": "Include -skipbaseline (default true)."},
                "includeThreadScopes": {"type": "boolean", "description": "After SummarizeTrace, also export ThreadScopes.csv via UnrealInsights (default false)."},
                "insightsPath": {"type": "string", "description": "Optional UnrealInsights.exe path used when includeThreadScopes is true."},
                "threadNames": {
                    "type": "array",
                    "items": {"type": "string"},
                    "description": "Optional thread names for thread scope export. Defaults to GameThread, RenderThread 0, RHIThread.",
                },
                "timeStartSeconds": {"type": "number", "description": "Optional timing-event export window start (seconds)."},
                "timeEndSeconds": {"type": "number", "description": "Optional timing-event export window end (seconds)."},
                "threadExportTimeoutSeconds": {"type": "integer", "description": "Timeout for UnrealInsights thread exports (default 900)."},
                "extraArgs": {"type": "array", "items": {"type": "string"}, "description": "Optional extra command-line arguments passed through to UnrealEditor-Cmd."},
            },
            "required": ["tracePath"],
        },
    ),
    Tool(
        name="unreal_insights_summarize_thread_scopes",
        description="Export per-thread timing events via UnrealInsights and aggregate them into ThreadScopes.csv for thread-aware hotspot analysis.",
        inputSchema={
            "type": "object",
            "properties": {
                "tracePath": {"type": "string", "description": "Absolute path to the .utrace file."},
                "insightsPath": {"type": "string", "description": "Optional absolute path to UnrealInsights.exe."},
                "engineRoot": {"type": "string", "description": "Optional Unreal Engine root used to resolve UnrealInsights.exe."},
                "threadNames": {
                    "type": "array",
                    "items": {"type": "string"},
                    "description": "Optional thread names to export. Defaults to GameThread, RenderThread 0, RHIThread.",
                },
                "timeStartSeconds": {"type": "number", "description": "Optional export window start time in seconds."},
                "timeEndSeconds": {"type": "number", "description": "Optional export window end time in seconds."},
                "timeoutSeconds": {"type": "integer", "description": "Timeout for UnrealInsights exports (default 900)."},
            },
            "required": ["tracePath"],
        },
    ),
    Tool(
        name="unreal_insights_analyze_trace",
        description="Analyze existing CSV summaries for a .utrace file and report global hotspots, bookmark clusters, thread ownership, and likely stutter patterns.",
        inputSchema={
            "type": "object",
            "properties": {
                "tracePath": {"type": "string", "description": "Absolute path to the original .utrace file. CSVs are resolved by the same filename stem."},
            },
            "required": ["tracePath"],
        },
    ),
    Tool(
        name="unreal_insights_analyze_frame_window",
        description="Analyze a focused frame or time window from CSV summaries. Best for rough mapping, bookmark clustering, and detecting streaming or terrain bursts near a hitch window.",
        inputSchema={
            "type": "object",
            "properties": {
                "tracePath": {"type": "string", "description": "Absolute path to the original .utrace file."},
                "frameStart": {"type": "integer", "description": "Start frame number."},
                "frameEnd": {"type": "integer", "description": "End frame number. If omitted, frameStart is used."},
                "timeStartSeconds": {"type": "number", "description": "Explicit start time in seconds. Use this when you already know the time window."},
                "timeEndSeconds": {"type": "number", "description": "Explicit end time in seconds."},
            },
            "required": ["tracePath"],
        },
    ),
    Tool(
        name="unreal_insights_explain_stutter",
        description="Return a conclusion-oriented explanation for a whole trace or a focused frame/time window. Useful when the caller wants the likely root-cause category instead of raw hotspot lists.",
        inputSchema={
            "type": "object",
            "properties": {
                "tracePath": {"type": "string", "description": "Absolute path to the original .utrace file."},
                "frameStart": {"type": "integer"},
                "frameEnd": {"type": "integer"},
                "timeStartSeconds": {"type": "number"},
                "timeEndSeconds": {"type": "number"},
            },
            "required": ["tracePath"],
        },
    ),
]


server = Server("ue-editor-mcp-insights")


@server.list_tools()
async def list_tools() -> list[Tool]:
    return TOOLS


@server.call_tool()
async def call_tool(name: str, arguments: dict[str, Any]) -> list[TextContent]:
    handlers = {
        "unreal_insights_summarize_trace": _handle_summarize_trace,
        "unreal_insights_summarize_thread_scopes": _handle_summarize_thread_scopes,
        "unreal_insights_analyze_trace": _handle_analyze_trace,
        "unreal_insights_analyze_frame_window": _handle_analyze_frame_window,
        "unreal_insights_explain_stutter": _handle_explain_stutter_pattern,
    }
    _aliases = {
        "unreal.insights.summarize_trace": "unreal_insights_summarize_trace",
        "unreal.insights.summarize_thread_scopes": "unreal_insights_summarize_thread_scopes",
        "unreal.insights.analyze_trace": "unreal_insights_analyze_trace",
        "unreal.insights.analyze_frame_window": "unreal_insights_analyze_frame_window",
        "unreal.insights.explain_stutter_pattern": "unreal_insights_explain_stutter",
        # Long name exceeded Cursor combined server+tool limit (60)
        "unreal_insights_explain_stutter_pattern": "unreal_insights_explain_stutter",
    }
    handler = handlers.get(_aliases.get(name, name))
    if handler is None:
        return _to_text({"success": False, "error": f"Unknown tool: {name}"})

    try:
        result = handler(arguments or {})
    except Exception as exc:
        logger.exception("%s failed", name)
        result = {"success": False, "error": str(exc), "notes": ["unexpected exception"]}
    return _to_text(result)


async def main() -> None:
    logging.basicConfig(level=logging.INFO)
    async with stdio_server() as streams:
        await server.run(
            streams[0],
            streams[1],
            server.create_initialization_options(),
        )


if __name__ == "__main__":
    asyncio.run(main())
