//! Parser tests against captured UE log fixtures.
//!
//! Fixtures are currently seeded from documented UE 5.7-era formats; they get
//! re-captured from the real UE 5.8 toolchain once the test project exists
//! (phase 2) — see the plan's "log-format drift" risk.

use mcp_unreal::headless::parsers::{parse_build_diagnostics, parse_test_list, parse_test_results};

const BUILD_LOG: &str = include_str!("fixtures/build_errors.log");
const TEST_RUN_LOG: &str = include_str!("fixtures/test_run.log");
const TEST_LIST_LOG: &str = include_str!("fixtures/test_list.log");

#[test]
fn build_diagnostics_finds_msvc_clang_and_ubt_errors_deduped() {
    let d = parse_build_diagnostics(BUILD_LOG);
    // MSVC error (deduped from 2 identical lines), MSVC fatal, clang error, UBT ERROR line.
    assert_eq!(d.errors.len(), 4, "errors: {:#?}", d.errors);
    assert!(d.errors[0].contains("error C2065"));
    assert!(d.errors.iter().any(|e| e.contains("fatal error C1004")));
    assert!(
        d.errors
            .iter()
            .any(|e| e.contains("undeclared identifier 'Foo'"))
    );
    assert!(d.errors.iter().any(|e| e.starts_with("ERROR:")));
    assert_eq!(d.warnings.len(), 1);
    assert!(d.warnings[0].contains("warning C4101"));
}

#[test]
fn test_results_parse_status_name_duration_and_events() {
    let run = parse_test_results(TEST_RUN_LOG);
    assert_eq!(run.results.len(), 4);
    assert_eq!(run.failed, 1);
    assert_eq!(run.passed, 2); // Passed + Success (legacy)
    assert_eq!(run.skipped, 1);

    let failed = &run.results[0];
    assert_eq!(failed.name, "MyProject.Unit.FirstTest"); // Path preferred over Name
    assert_eq!(failed.status, "fail");
    assert_eq!(failed.duration_secs, Some(0.05));
    // Both the Error and Warning events preceding the failure attach to it.
    assert_eq!(failed.events.len(), 2);
    assert!(failed.events[0].contains("Expected 5 but got 4"));

    let passed = &run.results[1];
    assert_eq!(passed.status, "pass");
    assert!(passed.events.is_empty());

    // Legacy Test={...} format still parses.
    let legacy = &run.results[3];
    assert_eq!(legacy.name, "MyProject.Legacy.OldFormatTest");
    assert_eq!(legacy.status, "pass");
}

#[test]
fn test_list_dedupes_and_requires_dotted_names() {
    let names = parse_test_list(TEST_LIST_LOG);
    assert_eq!(
        names,
        vec![
            "MyProject.Unit.FirstTest".to_string(),
            "MyProject.Unit.SecondTest".to_string(),
            "System.Core.Misc.Timespan".to_string(),
        ]
    );
}

#[test]
fn empty_log_yields_empty_results_not_panic() {
    let run = parse_test_results("");
    assert_eq!(run.results.len(), 0);
    let d = parse_build_diagnostics("");
    assert!(d.errors.is_empty() && d.warnings.is_empty());
    assert!(parse_test_list("").is_empty());
}

// ---- real UE 5.8 output, captured from the test project's own McpLink tests ----

const REAL_RUN_LOG: &str = include_str!("fixtures/test_run_ue58.log");
const REAL_LIST_LOG: &str = include_str!("fixtures/test_list_ue58.log");

#[test]
fn real_ue58_run_log_parses_all_eight_tests() {
    let run = parse_test_results(REAL_RUN_LOG);
    assert_eq!(
        run.results.len(),
        8,
        "names: {:?}",
        run.results.iter().map(|r| &r.name).collect::<Vec<_>>()
    );
    assert_eq!(run.passed, 8);
    assert_eq!(run.failed, 0);
    // 5.8 reports Result={Success} and puts the full dotted path in Path={...}.
    assert!(run.results.iter().all(|r| r.status == "pass"));
    assert!(
        run.results
            .iter()
            .any(|r| r.name == "McpLink.Core.Responder.ExactlyOnce")
    );
    assert!(run.results.iter().all(|r| r.name.starts_with("McpLink.")));
}

#[test]
fn real_ue58_list_log_yields_exact_names() {
    let names = parse_test_list(REAL_LIST_LOG);
    assert_eq!(
        names,
        vec![
            "McpLink.Blueprint.PinType",
            "McpLink.Core.Json.ParseBody",
            "McpLink.Core.LogCapture.Ring",
            "McpLink.Core.Resolve.Class",
            "McpLink.Core.Resolve.Vectors",
            "McpLink.Core.Responder.ExactlyOnce",
            "McpLink.Input.State.AxesAndMouse",
            "McpLink.Input.State.Keys",
        ]
        .into_iter()
        .map(String::from)
        .collect::<Vec<_>>()
    );
}

#[test]
fn events_after_completion_attach_to_the_right_test() {
    let log = "\
[..][ 1]LogAutomationController: Display: Test Completed. Result={Fail} Name={A} Path={X.A}
[..][ 1]LogAutomationController: BeginEvents: X.A
[..][ 1]LogAutomationController: Error: expected 1 got 2
[..][ 1]LogAutomationController: EndEvents: X.A
[..][ 2]LogAutomationController: Display: Test Completed. Result={Success} Name={B} Path={X.B}
[..][ 2]LogAutomationController: BeginEvents: X.B
[..][ 2]LogAutomationController: Warning: slow path
[..][ 2]LogAutomationController: EndEvents: X.B
";
    let run = parse_test_results(log);
    assert_eq!(run.failed, 1);
    assert_eq!(run.passed, 1);
    assert_eq!(run.results[0].events, vec!["expected 1 got 2".to_string()]);
    assert_eq!(
        run.results[1].events,
        vec!["slow path".to_string()],
        "warnings on passing tests are kept"
    );
}
