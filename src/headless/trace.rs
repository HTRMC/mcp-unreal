//! `trace_ops` — reading Unreal Insights traces back.
//!
//! `perf_ops trace_start`/`trace_stop` produce a .utrace; without a way to
//! analyse it that is a write-only file. UnrealInsights.exe can run headless
//! (`-OpenTraceFile -NoUI -AutoQuit`) and execute its own export commands on
//! analysis completion, which turns a trace into CSV this tool can summarise.

use std::path::{Path, PathBuf};
use std::time::Duration;

use rmcp::handler::server::wrapper::{Json, Parameters};
use rmcp::{ErrorData, tool, tool_router};
use serde_json::{Value, json};

use crate::budget::last_n_lines;
use crate::headless::runner;
use crate::{UnrealMcp, error};

const ANALYSIS_TIMEOUT: Duration = Duration::from_secs(30 * 60);
const DEFAULT_ROWS: usize = 40;
const MAX_ROWS: usize = 500;

/// The Insights export commands, keyed by the `data` value agents pass.
const EXPORTS: &[(&str, &str, &str)] = &[
    (
        "timers",
        "TimingInsights.ExportTimers",
        "every timer in the trace: id, name, type",
    ),
    (
        "timer_statistics",
        "TimingInsights.ExportTimerStatistics",
        "aggregated per-timer cost — instance count, inclusive and exclusive time. The one to start with when asking what is slow",
    ),
    (
        "timing_events",
        "TimingInsights.ExportTimingEvents",
        "every individual scope instance; huge, so narrow it with -threads=/-timers=/-startTime=/-endTime=",
    ),
    (
        "threads",
        "TimingInsights.ExportThreads",
        "thread ids and names",
    ),
    (
        "counters",
        "TimingInsights.ExportCounters",
        "the counters the trace recorded",
    ),
    (
        "counter_values",
        "TimingInsights.ExportCounterValues",
        "sampled counter values over time",
    ),
    (
        "timer_callees",
        "TimingInsights.ExportTimerCallees",
        "the callee tree under one timer; needs -timer=<name>",
    ),
];

#[derive(serde::Deserialize, schemars::JsonSchema)]
#[serde(tag = "operation", rename_all = "snake_case")]
#[schemars(transform = crate::schema::object_with_oneof)]
pub enum TraceOp {
    /// Traces on disk — the shared UnrealTrace store plus the project's
    /// Saved/Profiling folder — newest first.
    List { max_results: Option<u32> },
    /// The export kinds `export` accepts, and what each contains.
    ListExports {},
    /// Analyse a trace headlessly and export one data set to CSV, returning
    /// the first rows inline plus the file path for the rest.
    Export {
        /// .utrace path, or a file name from `list`. Defaults to the newest trace.
        trace: Option<String>,
        /// What to export; see list_exports. Default "timer_statistics".
        data: Option<String>,
        /// Output CSV path. Defaults into the project's Saved/Profiling folder.
        output: Option<String>,
        /// Extra options for the Insights command, e.g.
        /// "-threads=GameThread -timers=* -startTime=10 -endTime=20".
        options: Option<String>,
        /// Rows to return inline (default 40, max 500). The full CSV is on disk.
        max_rows: Option<u32>,
    },
    /// Run arbitrary Insights console commands against a trace — the escape
    /// hatch for anything `export` does not model.
    Run {
        trace: Option<String>,
        /// e.g. ["TimingInsights.ExportTimers C:/out/timers.csv"].
        commands: Vec<String>,
    },
}

#[tool_router(router = headless_trace_router, vis = "pub(crate)")]
impl UnrealMcp {
    #[tool(
        description = "Read Unreal Insights traces back: list the .utrace files on disk, then analyse one headlessly and export timers, aggregated timer statistics (inclusive/exclusive cost — the place to start when something is slow), individual timing events, threads or counters to CSV, with the first rows returned inline. Complements perf_ops trace_start/trace_stop, which only record."
    )]
    async fn trace_ops(
        &self,
        Parameters(op): Parameters<TraceOp>,
    ) -> Result<Json<Value>, ErrorData> {
        match op {
            TraceOp::ListExports {} => Ok(Json(json!({
                "exports": EXPORTS
                    .iter()
                    .map(|(name, command, description)| json!({
                        "data": name, "command": command, "description": description,
                    }))
                    .collect::<Vec<_>>(),
            }))),
            TraceOp::List { max_results } => {
                let limit = max_results.unwrap_or(30).clamp(1, 200) as usize;
                let traces = self.find_traces();
                Ok(Json(json!({
                    "count": traces.len(),
                    "search_paths": self.trace_dirs()
                        .iter()
                        .map(|d| d.display().to_string())
                        .collect::<Vec<_>>(),
                    "traces": traces
                        .iter()
                        .take(limit)
                        .map(|t| json!({
                            "name": t.file_name().unwrap_or_default().to_string_lossy(),
                            "path": t.display().to_string(),
                            "size_bytes": std::fs::metadata(t).map(|m| m.len()).unwrap_or(0),
                        }))
                        .collect::<Vec<_>>(),
                })))
            }
            TraceOp::Export {
                trace,
                data,
                output,
                options,
                max_rows,
            } => {
                let kind = data.unwrap_or_else(|| "timer_statistics".into());
                let Some((_, command, _)) = EXPORTS.iter().find(|(name, _, _)| *name == kind)
                else {
                    return Err(error::invalid(format!(
                        "unknown data '{kind}' — one of {}",
                        EXPORTS
                            .iter()
                            .map(|(n, _, _)| *n)
                            .collect::<Vec<_>>()
                            .join(", ")
                    )));
                };
                let trace = self.resolve_trace(trace.as_deref())?;
                let csv = match output {
                    Some(path) => PathBuf::from(path),
                    None => self.default_output(&trace, &kind)?,
                };
                if let Some(parent) = csv.parent() {
                    std::fs::create_dir_all(parent).map_err(|e| {
                        error::internal(format!("cannot create {}: {e}", parent.display()))
                    })?;
                }
                // A stale file from a previous run would otherwise be reported
                // as this run's output when the analysis fails.
                std::fs::remove_file(&csv).ok();

                let full = match options.as_deref().filter(|o| !o.is_empty()) {
                    Some(extra) => format!("{command} {} {extra}", csv.display()),
                    None => format!("{command} {}", csv.display()),
                };
                let run = self
                    .run_insights(&trace, std::slice::from_ref(&full))
                    .await?;
                let rows = read_csv(&csv, max_rows.unwrap_or(DEFAULT_ROWS as u32) as usize);
                Ok(Json(json!({
                    "trace": trace.display().to_string(),
                    "data": kind,
                    "command": full,
                    "csv": csv.display().to_string(),
                    "exists": csv.exists(),
                    "success": run["success"],
                    "duration_secs": run["duration_secs"],
                    "rows": rows.rows,
                    "row_count": rows.total,
                    "header": rows.header,
                    "log_tail": run["log_tail"],
                })))
            }
            TraceOp::Run { trace, commands } => {
                if commands.is_empty() {
                    return Err(error::invalid(
                        "'commands' must not be empty — see trace_ops list_exports",
                    ));
                }
                let trace = self.resolve_trace(trace.as_deref())?;
                let run = self.run_insights(&trace, &commands).await?;
                Ok(Json(json!({
                    "trace": trace.display().to_string(),
                    "commands": commands,
                    "success": run["success"],
                    "duration_secs": run["duration_secs"],
                    "log_tail": run["log_tail"],
                })))
            }
        }
    }

    fn insights_binary(&self) -> PathBuf {
        let bin = self.cfg.engine_root.join("Engine").join("Binaries");
        if cfg!(windows) {
            bin.join("Win64").join("UnrealInsights.exe")
        } else if cfg!(target_os = "macos") {
            bin.join("Mac").join("UnrealInsights")
        } else {
            bin.join("Linux").join("UnrealInsights")
        }
    }

    /// Where UnrealTraceServer and the editor leave .utrace files.
    fn trace_dirs(&self) -> Vec<PathBuf> {
        let mut dirs = Vec::new();
        if let Some(project) = self.cfg.project.as_ref() {
            dirs.push(project.root.join("Saved").join("Profiling"));
        }
        dirs.extend(trace_store_dir());
        dirs
    }

    /// Every .utrace under the search paths, newest first.
    fn find_traces(&self) -> Vec<PathBuf> {
        let mut found: Vec<(std::time::SystemTime, PathBuf)> = Vec::new();
        for dir in self.trace_dirs() {
            let Ok(entries) = std::fs::read_dir(&dir) else {
                continue;
            };
            for entry in entries.filter_map(|e| e.ok()) {
                let path = entry.path();
                if path.extension().is_some_and(|e| e == "utrace") {
                    let modified = entry
                        .metadata()
                        .and_then(|m| m.modified())
                        .unwrap_or(std::time::UNIX_EPOCH);
                    found.push((modified, path));
                }
            }
        }
        found.sort_by_key(|(modified, _)| std::cmp::Reverse(*modified));
        found.into_iter().map(|(_, path)| path).collect()
    }

    fn resolve_trace(&self, spec: Option<&str>) -> Result<PathBuf, ErrorData> {
        match spec.filter(|s| !s.is_empty()) {
            Some(spec) => {
                let direct = PathBuf::from(spec);
                if direct.is_file() {
                    return Ok(direct);
                }
                self.find_traces()
                    .into_iter()
                    .find(|t| {
                        t.file_name()
                            .is_some_and(|n| n.to_string_lossy().eq_ignore_ascii_case(spec))
                    })
                    .ok_or_else(|| {
                        error::invalid(format!(
                            "no trace '{spec}' — pass a full path, or a file name from trace_ops list"
                        ))
                    })
            }
            None => self.find_traces().into_iter().next().ok_or_else(|| {
                error::invalid(
                    "no .utrace files found — record one with perf_ops trace_start / trace_stop, \
                     or pass `trace` with a path",
                )
            }),
        }
    }

    fn default_output(&self, trace: &Path, kind: &str) -> Result<PathBuf, ErrorData> {
        let stem = trace
            .file_stem()
            .map(|s| s.to_string_lossy().into_owned())
            .unwrap_or_else(|| "trace".into());
        let dir = match self.cfg.project.as_ref() {
            Some(project) => project.root.join("Saved").join("Profiling"),
            None => trace.parent().map(Path::to_path_buf).ok_or_else(|| {
                error::invalid("pass `output` — the trace has no parent directory to write beside")
            })?,
        };
        Ok(dir.join(format!("{stem}-{kind}.csv")))
    }

    /// Analyse `trace` headlessly, running `commands` once analysis completes.
    async fn run_insights(&self, trace: &Path, commands: &[String]) -> Result<Value, ErrorData> {
        let binary = self.insights_binary();
        if !binary.exists() {
            return Err(error::missing_tool(&binary, "UnrealInsights"));
        }
        // -ExecOnAnalysisCompleteCmd takes one command, or "@=<file>" to read
        // a list from disk, so multiple commands go through a scratch file.
        let (exec_arg, command_file) = if commands.len() == 1 {
            (commands[0].clone(), None)
        } else {
            let path = std::env::temp_dir()
                .join(format!("mcp-unreal-insights-{}.txt", std::process::id()));
            std::fs::write(&path, commands.join("\n"))
                .map_err(|e| error::internal(format!("cannot write {}: {e}", path.display())))?;
            (format!("@={}", path.display()), Some(path))
        };

        let args = vec![
            format!("-OpenTraceFile={}", trace.display()),
            "-NoUI".to_string(),
            "-AutoQuit".to_string(),
            "-Unattended".to_string(),
            "-NoSplash".to_string(),
            format!("-ExecOnAnalysisCompleteCmd={exec_arg}"),
            "-log".to_string(),
            "-stdout".to_string(),
            "-FullStdOutLogOutput".to_string(),
        ];
        let result = runner::run(&binary, &args, ANALYSIS_TIMEOUT)
            .await
            .map_err(|e| error::internal(e.to_string()))?;
        if let Some(path) = command_file {
            std::fs::remove_file(path).ok();
        }
        let combined = result.combined();
        Ok(json!({
            "success": !result.timed_out && result.exit_code == 0,
            "exit_code": result.exit_code,
            "timed_out": result.timed_out,
            "duration_secs": result.duration_secs,
            "log_tail": last_n_lines(&combined, 30),
        }))
    }
}

/// The shared UnrealTrace store the trace server writes into.
pub(crate) fn trace_store_dir() -> Option<PathBuf> {
    let base = if cfg!(windows) {
        dirs::data_local_dir()?.join("UnrealEngine").join("Common")
    } else {
        dirs::home_dir()?.join("UnrealEngine")
    };
    Some(base.join("UnrealTrace").join("Store").join("001"))
}

pub(crate) struct CsvPreview {
    pub header: Option<String>,
    pub rows: Vec<String>,
    /// Data rows in the file, excluding the header.
    pub total: usize,
}

/// First `max` data rows of a CSV, with the header split out. A missing file
/// yields an empty preview rather than an error — the caller reports `exists`
/// and the analysis log, which say far more about what went wrong.
pub(crate) fn read_csv(path: &Path, max: usize) -> CsvPreview {
    let Ok(text) = std::fs::read_to_string(path) else {
        return CsvPreview {
            header: None,
            rows: Vec::new(),
            total: 0,
        };
    };
    let mut lines = text.lines();
    let header = lines.next().map(str::to_string);
    let rows: Vec<&str> = lines.collect();
    CsvPreview {
        total: rows.len(),
        rows: rows
            .into_iter()
            .take(max.clamp(1, MAX_ROWS))
            .map(str::to_string)
            .collect(),
        header,
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn every_export_kind_maps_to_an_insights_command() {
        for (name, command, description) in EXPORTS {
            assert!(!name.is_empty());
            assert!(command.starts_with("TimingInsights."), "{command}");
            assert!(!description.is_empty(), "{name} needs a description");
        }
        assert!(EXPORTS.iter().any(|(n, _, _)| *n == "timer_statistics"));
    }

    #[test]
    fn csv_preview_splits_the_header_and_caps_rows() {
        let dir = std::env::temp_dir().join(format!("mcp-trace-{}", std::process::id()));
        std::fs::create_dir_all(&dir).unwrap();
        let path = dir.join("t.csv");
        std::fs::write(&path, "Name,Time\na,1\nb,2\nc,3\n").unwrap();

        let preview = read_csv(&path, 2);
        assert_eq!(preview.header.as_deref(), Some("Name,Time"));
        assert_eq!(preview.rows, vec!["a,1", "b,2"]);
        assert_eq!(preview.total, 3);
        std::fs::remove_dir_all(&dir).ok();
    }

    #[test]
    fn a_missing_csv_previews_as_empty() {
        let preview = read_csv(Path::new("no/such.csv"), 10);
        assert!(preview.header.is_none());
        assert_eq!(preview.total, 0);
    }
}
