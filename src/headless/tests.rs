//! `run_tests`, `run_visual_tests`, `list_tests` — headless UE automation tests.

use std::time::Duration;

use rmcp::handler::server::wrapper::{Json, Parameters};
use rmcp::{ErrorData, tool, tool_router};

use crate::budget::last_n_lines;
use crate::headless::parsers::{TestResult, parse_test_list, parse_test_results};
use crate::headless::runner;
use crate::{UnrealMcp, error};

const TEST_TIMEOUT: Duration = Duration::from_secs(30 * 60);
const LIST_TIMEOUT: Duration = Duration::from_secs(10 * 60);
const LOG_TAIL_LINES: usize = 30;

#[derive(serde::Deserialize, schemars::JsonSchema)]
pub struct RunTestsInput {
    /// Test name filter (substring / prefix), e.g. "MyProject.Unit". Empty runs all tests.
    pub filter: Option<String>,
}

#[derive(serde::Serialize, schemars::JsonSchema)]
pub struct RunTestsOutput {
    /// True when at least one test ran and none failed. False with tests_found=0 usually
    /// means the run crashed or produced no parseable results — check log_tail.
    pub success: bool,
    pub tests_found: usize,
    pub passed: usize,
    pub failed: usize,
    pub skipped: usize,
    pub timed_out: bool,
    pub duration_secs: u64,
    pub results: Vec<TestResult>,
    pub log_tail: String,
}

#[derive(serde::Deserialize, schemars::JsonSchema)]
pub struct ListTestsInput {
    /// Case-insensitive substring filter on test names.
    pub filter: Option<String>,
    /// Maximum names to return (default 200).
    pub max_results: Option<usize>,
}

#[derive(serde::Serialize, schemars::JsonSchema)]
pub struct ListTestsOutput {
    pub total: usize,
    pub tests: Vec<String>,
    pub timed_out: bool,
}

/// If the captured stdout carries none of the expected automation output,
/// append the editor's own log file. The editor always writes
/// `Saved/Logs/<Project>.log`; stdout depends on flags and can be swamped by
/// child processes, so this keeps the parsers fed either way.
fn with_editor_log_fallback(
    mut combined: String,
    project: &crate::config::ProjectInfo,
    marker: &str,
) -> String {
    if combined.contains(marker) {
        return combined;
    }
    let log_path = project
        .root
        .join("Saved")
        .join("Logs")
        .join(format!("{}.log", project.name));
    match std::fs::read_to_string(&log_path) {
        Ok(log) => {
            tracing::debug!("stdout lacked {marker}; using {}", log_path.display());
            combined.push('\n');
            combined.push_str(&log);
        }
        Err(e) => tracing::warn!(
            "stdout lacked {marker} and {} is unreadable: {e}",
            log_path.display()
        ),
    }
    combined
}

impl UnrealMcp {
    async fn run_automation_tests(
        &self,
        filter: Option<String>,
        null_rhi: bool,
    ) -> Result<Json<RunTestsOutput>, ErrorData> {
        let project = self.cfg.project.as_ref().ok_or_else(error::no_project)?;
        if !self.cfg.editor_cmd.exists() {
            return Err(error::no_editor_binary(&self.cfg.editor_cmd));
        }
        let filter = match filter.as_deref() {
            None | Some("") => ".".to_string(),
            Some(f) => f.to_string(),
        };
        let mut args = vec![
            project.uproject.display().to_string(),
            // Don't chain ";Quit": it races the final test and its result is
            // lost. -TestExit makes the engine leave once the automation
            // framework logs that its queue is empty.
            format!("-ExecCmds=Automation RunTests {filter}"),
            "-TestExit=Automation Test Queue Empty".into(),
            "-unattended".into(),
            "-nopause".into(),
            "-nosplash".into(),
            "-nosound".into(),
            // Without these the editor keeps its log in Saved/Logs only and
            // stdout carries nothing but child-process (UBT) chatter.
            "-stdout".into(),
            "-FullStdOutLogOutput".into(),
        ];
        if null_rhi {
            args.push("-nullrhi".into());
        }
        let result = runner::run(&self.cfg.editor_cmd, &args, TEST_TIMEOUT)
            .await
            .map_err(|e| error::internal(e.to_string()))?;
        let combined =
            with_editor_log_fallback(result.combined(), project, "LogAutomationController");
        let run = parse_test_results(&combined);
        let tests_found = run.results.len();
        Ok(Json(RunTestsOutput {
            success: !result.timed_out && run.failed == 0 && tests_found > 0,
            tests_found,
            passed: run.passed,
            failed: run.failed,
            skipped: run.skipped,
            timed_out: result.timed_out,
            duration_secs: result.duration_secs,
            results: run.results,
            log_tail: last_n_lines(&combined, LOG_TAIL_LINES),
        }))
    }
}

#[tool_router(router = headless_test_router, vis = "pub(crate)")]
impl UnrealMcp {
    #[tool(
        description = "Run UE automation tests headless (-nullrhi, no GPU). Returns per-test pass/fail with failure events. No editor needs to be open. Takes minutes."
    )]
    async fn run_tests(
        &self,
        Parameters(input): Parameters<RunTestsInput>,
    ) -> Result<Json<RunTestsOutput>, ErrorData> {
        self.run_automation_tests(input.filter, true).await
    }

    #[tool(
        description = "Run UE automation tests with GPU rendering enabled (no -nullrhi) for visual/rendering tests. Slower than run_tests."
    )]
    async fn run_visual_tests(
        &self,
        Parameters(input): Parameters<RunTestsInput>,
    ) -> Result<Json<RunTestsOutput>, ErrorData> {
        self.run_automation_tests(input.filter, false).await
    }

    #[tool(
        description = "List available UE automation test names, optionally filtered. Launches the editor headless briefly — takes a minute or two."
    )]
    async fn list_tests(
        &self,
        Parameters(input): Parameters<ListTestsInput>,
    ) -> Result<Json<ListTestsOutput>, ErrorData> {
        let project = self.cfg.project.as_ref().ok_or_else(error::no_project)?;
        if !self.cfg.editor_cmd.exists() {
            return Err(error::no_editor_binary(&self.cfg.editor_cmd));
        }
        let args = vec![
            project.uproject.display().to_string(),
            "-ExecCmds=Automation List;Quit".into(),
            "-nullrhi".into(),
            "-unattended".into(),
            "-nopause".into(),
            "-nosplash".into(),
            "-nosound".into(),
            "-stdout".into(),
            "-FullStdOutLogOutput".into(),
        ];
        let result = runner::run(&self.cfg.editor_cmd, &args, LIST_TIMEOUT)
            .await
            .map_err(|e| error::internal(e.to_string()))?;
        let combined =
            with_editor_log_fallback(result.combined(), project, "LogAutomationCommandLine");
        let mut tests = parse_test_list(&combined);
        if let Some(filter) = input.filter.as_deref().filter(|f| !f.is_empty()) {
            let needle = filter.to_ascii_lowercase();
            tests.retain(|t| t.to_ascii_lowercase().contains(&needle));
        }
        let total = tests.len();
        tests.truncate(input.max_results.unwrap_or(200));
        Ok(Json(ListTestsOutput {
            total,
            tests,
            timed_out: result.timed_out,
        }))
    }
}
