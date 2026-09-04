//! `get_test_log` — read raw UE log files from the project's Saved/Logs.

use rmcp::handler::server::wrapper::{Json, Parameters};
use rmcp::{ErrorData, tool, tool_router};

use crate::{UnrealMcp, error};

const DEFAULT_LINES: usize = 200;
const MAX_LINES: usize = 500;

#[derive(serde::Deserialize, schemars::JsonSchema)]
pub struct GetTestLogInput {
    /// Log file name inside Saved/Logs (no paths). Defaults to the most recently modified .log.
    pub file: Option<String>,
    /// Number of lines to return from the end (default 200, hard cap 500).
    pub lines: Option<usize>,
    /// Only return lines containing this case-insensitive substring.
    pub filter: Option<String>,
}

#[derive(serde::Serialize, schemars::JsonSchema)]
pub struct GetTestLogOutput {
    pub file: String,
    pub total_lines: usize,
    pub returned_lines: usize,
    pub content: String,
}

#[tool_router(router = headless_log_router, vis = "pub(crate)")]
impl UnrealMcp {
    #[tool(
        description = "Read a raw UE log file from the project's Saved/Logs directory (most recent by default), tail-limited and optionally filtered."
    )]
    async fn get_test_log(
        &self,
        Parameters(input): Parameters<GetTestLogInput>,
    ) -> Result<Json<GetTestLogOutput>, ErrorData> {
        let project = self.cfg.project.as_ref().ok_or_else(error::no_project)?;
        let logs_dir = project.root.join("Saved").join("Logs");

        let path = match input.file {
            Some(name) => {
                if name.contains('/') || name.contains('\\') || name.contains("..") {
                    return Err(error::invalid(
                        "file must be a bare log file name inside Saved/Logs, not a path",
                    ));
                }
                logs_dir.join(name)
            }
            None => latest_log(&logs_dir).ok_or_else(|| {
                error::invalid(format!(
                    "no .log files found in {} — run a build or test first",
                    logs_dir.display()
                ))
            })?,
        };

        let content = std::fs::read_to_string(&path)
            .map_err(|e| error::invalid(format!("cannot read {}: {e}", path.display())))?;

        let all: Vec<&str> = content.lines().collect();
        let total_lines = all.len();
        let wanted = input.lines.unwrap_or(DEFAULT_LINES).min(MAX_LINES);

        let selected: Vec<&str> = match input.filter.as_deref().filter(|f| !f.is_empty()) {
            Some(f) => {
                let needle = f.to_ascii_lowercase();
                let matching: Vec<&str> = all
                    .iter()
                    .filter(|l| l.to_ascii_lowercase().contains(&needle))
                    .copied()
                    .collect();
                let start = matching.len().saturating_sub(wanted);
                matching[start..].to_vec()
            }
            None => {
                let start = all.len().saturating_sub(wanted);
                all[start..].to_vec()
            }
        };

        Ok(Json(GetTestLogOutput {
            file: path.display().to_string(),
            total_lines,
            returned_lines: selected.len(),
            content: selected.join("\n"),
        }))
    }
}

fn latest_log(dir: &std::path::Path) -> Option<std::path::PathBuf> {
    let entries = std::fs::read_dir(dir).ok()?;
    entries
        .filter_map(|e| e.ok())
        .filter(|e| e.path().extension().is_some_and(|x| x == "log"))
        .max_by_key(|e| e.metadata().and_then(|m| m.modified()).ok())
        .map(|e| e.path())
}
