//! Parsers turning raw UE / UBT log text into structured results.
//!
//! Regexes are seeded from the UE 5.7-era formats and validated against real
//! UE 5.8 fixtures in `tests/parser_tests.rs`; parsing is always best-effort —
//! callers surface a raw `log_tail` regardless so a format drift degrades to
//! "raw text", never to silence.

use std::collections::HashSet;
use std::sync::LazyLock;

use regex::Regex;

/// MSVC/clang-style `path(line): error C1234: ...`
static MSVC_ERROR_RE: LazyLock<Regex> =
    LazyLock::new(|| Regex::new(r"(?m)^.+\(\d+\)\s*:\s*(?:fatal\s+)?error\s.*$").unwrap());
static MSVC_WARNING_RE: LazyLock<Regex> =
    LazyLock::new(|| Regex::new(r"(?m)^.+\(\d+\)\s*:\s*warning\s.*$").unwrap());
/// clang/gcc-style `path:line:col: error: ...`
static CLANG_ERROR_RE: LazyLock<Regex> =
    LazyLock::new(|| Regex::new(r"(?m)^.+:\d+:\d+:\s*(?:fatal\s+)?error:.*$").unwrap());
/// UBT/tool-level `ERROR: ...`
static TOOL_ERROR_RE: LazyLock<Regex> =
    LazyLock::new(|| Regex::new(r"(?m)^\s*ERROR:\s.*$").unwrap());

#[derive(Debug, Default)]
pub struct BuildDiagnostics {
    pub errors: Vec<String>,
    pub warnings: Vec<String>,
}

pub fn parse_build_diagnostics(log: &str) -> BuildDiagnostics {
    let mut seen = HashSet::new();
    let mut errors = Vec::new();
    for re in [&*MSVC_ERROR_RE, &*CLANG_ERROR_RE, &*TOOL_ERROR_RE] {
        for m in re.find_iter(log) {
            let line = m.as_str().trim().to_string();
            if seen.insert(line.clone()) {
                errors.push(line);
            }
        }
    }
    let mut seen_w = HashSet::new();
    let mut warnings = Vec::new();
    for m in MSVC_WARNING_RE.find_iter(log) {
        let line = m.as_str().trim().to_string();
        if seen_w.insert(line.clone()) {
            warnings.push(line);
        }
    }
    BuildDiagnostics { errors, warnings }
}

/// `LogAutomationController: ... Test Completed. Result={Passed} Name={..} Path={..}`
/// (5.7-era logs used `Test={..}`; both are accepted.)
static TEST_RESULT_RE: LazyLock<Regex> = LazyLock::new(|| {
    Regex::new(
        r"LogAutomationController.*?Test Completed\.\s*Result=\{(\w+)\}\s*(?:Test|Name)=\{([^}]*)\}(?:\s*Path=\{([^}]*)\})?",
    )
    .unwrap()
});
static TEST_DURATION_RE: LazyLock<Regex> =
    LazyLock::new(|| Regex::new(r"Test Completed.*?Duration=\{([^}]*)\}").unwrap());
static TEST_EVENT_RE: LazyLock<Regex> =
    LazyLock::new(|| Regex::new(r"LogAutomationController.*?(?:Error|Warning):\s*(.+)$").unwrap());
/// UE 5.8 prints each test's events after its completion line, bracketed by
/// `BeginEvents: <path>` / `EndEvents: <path>`.
static TEST_EVENTS_BEGIN_RE: LazyLock<Regex> =
    LazyLock::new(|| Regex::new(r"LogAutomationController.*?BeginEvents:\s*(\S.*?)\s*$").unwrap());
static TEST_EVENTS_END_RE: LazyLock<Regex> =
    LazyLock::new(|| Regex::new(r"LogAutomationController.*?EndEvents:").unwrap());
/// `Automation List` output: one indented name per line under
/// `LogAutomationCommandLine: Display: Found N automation tests ...`. Older
/// builds emitted `LogAutomationController: ... ] Name`; both are accepted.
static TEST_LIST_RE: LazyLock<Regex> = LazyLock::new(|| {
    Regex::new(r"LogAutomation(?:CommandLine|Controller):\s*(?:Display:)?\s*(?:\]\s*)?(\S.*?)\s*$")
        .unwrap()
});

#[derive(Debug, Clone, serde::Serialize, schemars::JsonSchema)]
pub struct TestResult {
    pub name: String,
    /// "pass" | "fail" | "skip"
    pub status: String,
    pub duration_secs: Option<f64>,
    /// Error/warning events emitted while this (failed) test ran.
    pub events: Vec<String>,
}

#[derive(Debug, Default)]
pub struct TestRun {
    pub results: Vec<TestResult>,
    pub passed: usize,
    pub failed: usize,
    pub skipped: usize,
}

/// Stateful single pass. Events inside a `BeginEvents`/`EndEvents` block are
/// attached to the test that block names (UE 5.8 prints them after the
/// completion line). Events seen outside any block accumulate and attach to
/// the next completed test if it failed (older log layout).
pub fn parse_test_results(log: &str) -> TestRun {
    let mut run = TestRun::default();
    let mut pending_events: Vec<String> = Vec::new();
    let mut events_block_for: Option<String> = None;

    for line in log.lines() {
        if let Some(caps) = TEST_EVENTS_BEGIN_RE.captures(line) {
            events_block_for = caps.get(1).map(|m| m.as_str().to_string());
            continue;
        }
        if TEST_EVENTS_END_RE.is_match(line) {
            events_block_for = None;
            continue;
        }
        if let Some(caps) = TEST_RESULT_RE.captures(line) {
            let raw_status = caps.get(1).map_or("", |m| m.as_str());
            // Prefer the full dotted Path={...} when present; older logs only
            // carry Test={...} / Name={...}.
            let name = caps
                .get(3)
                .map(|m| m.as_str().trim())
                .filter(|p| !p.is_empty())
                .unwrap_or_else(|| caps.get(2).map_or("", |m| m.as_str()).trim())
                .to_string();
            let status = normalize_status(raw_status);
            let duration_secs = TEST_DURATION_RE
                .captures(line)
                .and_then(|c| c.get(1))
                .and_then(|m| m.as_str().trim().parse::<f64>().ok());
            let events = if status == "fail" {
                std::mem::take(&mut pending_events)
            } else {
                pending_events.clear();
                Vec::new()
            };
            match status.as_str() {
                "pass" => run.passed += 1,
                "fail" => run.failed += 1,
                _ => run.skipped += 1,
            }
            run.results.push(TestResult {
                name,
                status,
                duration_secs,
                events,
            });
        } else if let Some(caps) = TEST_EVENT_RE.captures(line) {
            let Some(msg) = caps.get(1) else { continue };
            let msg = msg.as_str().trim().to_string();
            match &events_block_for {
                // Attach to the test the block names, whatever its status —
                // warnings on passing tests are worth surfacing too.
                Some(path) => {
                    if let Some(result) = run.results.iter_mut().rev().find(|r| &r.name == path) {
                        result.events.push(msg);
                    } else {
                        pending_events.push(msg);
                    }
                }
                None => pending_events.push(msg),
            }
        }
    }
    run
}

fn normalize_status(raw: &str) -> String {
    match raw.to_ascii_lowercase().as_str() {
        "failed" | "fail" => "fail".into(),
        "skipped" | "skip" | "notrun" => "skip".into(),
        _ => "pass".into(),
    }
}

/// Parse `Automation List` output into test names. A name must contain a `.`
/// and not be one of the framework's own status lines.
pub fn parse_test_list(log: &str) -> Vec<String> {
    let mut seen = HashSet::new();
    let mut names = Vec::new();
    for line in log.lines() {
        let Some(caps) = TEST_LIST_RE.captures(line) else {
            continue;
        };
        let mut name = caps.get(1).map_or("", |m| m.as_str()).trim().to_string();
        // Older builds prefix each entry with its index: `[12] Some.Test`.
        if name.starts_with('[') {
            if let Some(close) = name.find(']') {
                if name[1..close].chars().all(|c| c.is_ascii_digit()) {
                    name = name[close + 1..].trim().to_string();
                }
            }
        }
        let is_status_line = name.starts_with("Found ")
            || name.starts_with("Ready")
            || name.contains("based on")
            || name.contains("Test Started")
            || name.contains("Test Completed")
            || name.starts_with("Sending ")
            || name.starts_with("Received ")
            || name.starts_with("Requesting ")
            || name.starts_with("Clearing ")
            || name.starts_with("Ignoring ")
            || name.starts_with("BeginEvents")
            || name.starts_with("EndEvents")
            || name.ends_with(" available on")
            || name.contains(" tests available on ");
        if !is_status_line
            && name.contains('.')
            && !name.contains(": ")
            && seen.insert(name.clone())
        {
            names.push(name);
        }
    }
    names
}
