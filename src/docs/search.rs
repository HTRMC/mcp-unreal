//! Regex search across engine headers using ripgrep's engine. No index to
//! maintain — a warm-cache scan of the header corpus takes a couple of seconds,
//! comfortably inside a tool call, and results are exact source lines.

use std::path::{Path, PathBuf};
use std::sync::Mutex;
use std::sync::atomic::{AtomicUsize, Ordering};

use grep_regex::RegexMatcher;
use grep_searcher::sinks::UTF8;
use grep_searcher::{BinaryDetection, SearcherBuilder};
use ignore::{WalkBuilder, WalkState};

#[derive(Debug, serde::Serialize, schemars::JsonSchema)]
pub struct SearchHit {
    pub file: String,
    pub line: u64,
    pub text: String,
    pub module: String,
}

pub struct SearchOptions {
    pub max_results: usize,
    /// Restrict to headers whose owning module matches (case-insensitive).
    pub module: Option<String>,
    /// Restrict to headers whose path contains this fragment.
    pub path_contains: Option<String>,
}

/// Run `pattern` over engine headers. Returns hits and the total number found
/// before capping.
pub fn search(
    engine_root: &Path,
    pattern: &str,
    options: &SearchOptions,
) -> Result<(Vec<SearchHit>, usize), String> {
    let matcher = RegexMatcher::new_line_matcher(pattern)
        .map_err(|e| format!("invalid regex '{pattern}': {e}"))?;

    let engine = engine_root.join("Engine");
    let roots: Vec<PathBuf> = [
        engine.join("Source").join("Runtime"),
        engine.join("Source").join("Editor"),
        engine.join("Source").join("Developer"),
        engine.join("Plugins"),
    ]
    .into_iter()
    .filter(|p| p.exists())
    .collect();
    if roots.is_empty() {
        return Err(format!(
            "no engine source under {} — set UE_ENGINE_ROOT to a UE 5.8 install with source",
            engine.display()
        ));
    }

    let hits: Mutex<Vec<SearchHit>> = Mutex::new(Vec::new());
    let total = AtomicUsize::new(0);

    let mut builder = WalkBuilder::new(&roots[0]);
    for root in &roots[1..] {
        builder.add(root);
    }
    builder
        .standard_filters(false)
        .threads(std::thread::available_parallelism().map_or(4, |n| n.get()));

    builder.build_parallel().run(|| {
        let matcher = matcher.clone();
        let hits = &hits;
        let total = &total;
        let mut searcher = SearcherBuilder::new()
            .binary_detection(BinaryDetection::quit(b'\x00'))
            .line_number(true)
            .build();

        Box::new(move |entry| {
            let Ok(entry) = entry else {
                return WalkState::Continue;
            };
            let path = entry.path();
            let name = path.file_name().unwrap_or_default().to_string_lossy();
            if entry.file_type().is_some_and(|t| t.is_dir()) {
                if matches!(
                    name.as_ref(),
                    "ThirdParty" | "Intermediate" | "Binaries" | "Content" | "Resources"
                ) {
                    return WalkState::Skip;
                }
                return WalkState::Continue;
            }
            if !name.ends_with(".h") || name.ends_with(".generated.h") {
                return WalkState::Continue;
            }

            let module = super::class_index::module_of(path);
            if let Some(want) = &options.module {
                if !module.eq_ignore_ascii_case(want) {
                    return WalkState::Continue;
                }
            }
            if let Some(fragment) = &options.path_contains {
                if !path
                    .to_string_lossy()
                    .to_lowercase()
                    .contains(&fragment.to_lowercase())
                {
                    return WalkState::Continue;
                }
            }

            // Stop early once we have plenty; the count stays approximate but
            // an agent never needs an exact total for a source grep.
            if total.load(Ordering::Relaxed) > options.max_results * 20 {
                return WalkState::Quit;
            }

            let _ = searcher.search_path(
                &matcher,
                path,
                UTF8(|line_number, line| {
                    total.fetch_add(1, Ordering::Relaxed);
                    let mut guard = hits.lock().unwrap();
                    if guard.len() < options.max_results {
                        guard.push(SearchHit {
                            file: path.display().to_string(),
                            line: line_number,
                            text: line.trim_end().to_string(),
                            module: module.clone(),
                        });
                    }
                    Ok(true)
                }),
            );
            WalkState::Continue
        })
    });

    let mut found = hits.into_inner().unwrap();
    found.sort_by(|a, b| a.file.cmp(&b.file).then(a.line.cmp(&b.line)));
    Ok((found, total.load(Ordering::Relaxed)))
}
