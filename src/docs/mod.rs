//! Engine-source API lookup: `lookup_class` and `search_api`.
//!
//! These read the engine source of the *installed* build, so answers always
//! match the exact UE version in use — no curated docs to drift out of date.

pub mod class_index;
pub mod header_parser;
pub mod search;

use std::path::PathBuf;
use std::sync::Arc;

use rmcp::handler::server::wrapper::{Json, Parameters};
use rmcp::{ErrorData, tool, tool_router};
use tokio::sync::OnceCell;

use crate::config::Config;
use crate::{UnrealMcp, error};

use class_index::ClassIndex;

const MAX_MEMBERS: usize = 40;

/// Lazily built, disk-cached class index.
pub struct DocsIndex {
    cell: OnceCell<Arc<ClassIndex>>,
    cfg: Arc<Config>,
}

impl DocsIndex {
    pub fn new(cfg: Arc<Config>) -> Self {
        Self {
            cell: OnceCell::new(),
            cfg,
        }
    }

    fn cache_path(&self, engine_version: &str) -> PathBuf {
        self.cfg
            .docs_cache_dir
            .join(format!("class-index-{engine_version}.json"))
    }

    /// Build once per process; subsequent calls await the same result.
    pub async fn get(&self) -> Arc<ClassIndex> {
        self.cell
            .get_or_init(|| async {
                let version = crate::status::engine_version(&self.cfg.engine_root)
                    .unwrap_or_else(|| "unknown".to_string());
                let cache = self.cache_path(&version);

                if let Some(index) = ClassIndex::load(&cache, &version) {
                    tracing::info!(
                        "loaded cached class index ({} symbols) from {}",
                        index.symbols.len(),
                        cache.display()
                    );
                    return Arc::new(index);
                }

                let engine_root = self.cfg.engine_root.clone();
                tracing::info!(
                    "building engine class index from {} (first run, may take a minute)",
                    engine_root.display()
                );
                let started = std::time::Instant::now();
                // Blocking, CPU-bound file walk: keep it off the async runtime.
                let index =
                    tokio::task::spawn_blocking(move || ClassIndex::build(&engine_root, version))
                        .await
                        .unwrap_or_else(|e| {
                            tracing::error!("class index build panicked: {e}");
                            ClassIndex {
                                engine_version: "unknown".into(),
                                symbols: Default::default(),
                                headers_scanned: 0,
                            }
                        });
                tracing::info!(
                    "indexed {} symbols from {} headers in {:.1}s",
                    index.symbols.len(),
                    index.headers_scanned,
                    started.elapsed().as_secs_f32()
                );
                index.save(&cache);
                Arc::new(index)
            })
            .await
            .clone()
    }

    /// Whether the index is built and non-empty (for the `status` tool).
    pub fn is_ready(&self) -> bool {
        self.cell.get().is_some_and(|i| !i.symbols.is_empty())
    }
}

#[derive(serde::Deserialize, schemars::JsonSchema)]
pub struct LookupClassInput {
    /// UE type name, e.g. "ACharacter", "UCharacterMovementComponent", "FHitResult",
    /// "EInputEvent". The leading A/U/F/E is optional.
    pub class_name: String,
}

#[derive(serde::Serialize, schemars::JsonSchema)]
#[serde(untagged)]
pub enum LookupClassOutput {
    Found(Box<header_parser::ClassSummary>),
    NotFound {
        found: bool,
        searched: String,
        /// Close names from the index, if any.
        suggestions: Vec<String>,
    },
}

#[derive(serde::Deserialize, schemars::JsonSchema)]
pub struct SearchApiInput {
    /// Rust/RE2 regex to match against engine header lines,
    /// e.g. "InjectInputForAction", "virtual bool InputKey".
    pub pattern: String,
    /// Restrict to a module, e.g. "Engine", "UnrealEd", "EnhancedInput".
    pub module: Option<String>,
    /// Restrict to headers whose path contains this fragment, e.g. "GameFramework".
    pub path_contains: Option<String>,
    /// Max hits to return (default 30, cap 200).
    pub max_results: Option<usize>,
}

#[derive(serde::Serialize, schemars::JsonSchema)]
pub struct SearchApiOutput {
    pub hits: Vec<search::SearchHit>,
    /// Total matches found before capping (approximate for very common patterns).
    pub total: usize,
    pub truncated: bool,
}

#[tool_router(router = docs_router, vis = "pub(crate)")]
impl UnrealMcp {
    #[tool(
        description = "Look up a UE type in the installed engine's own source: parent class, module, header path, reflection specifiers, and the real UPROPERTY/UFUNCTION signatures. Use this before writing UE C++ or calling call_function so signatures match your exact engine version. The first call builds an index and may take up to a minute."
    )]
    async fn lookup_class(
        &self,
        Parameters(input): Parameters<LookupClassInput>,
    ) -> Result<Json<LookupClassOutput>, ErrorData> {
        let index = self.docs.get().await;
        if index.symbols.is_empty() {
            return Err(error::invalid(format!(
                "no engine source indexed under {} — set UE_ENGINE_ROOT to a UE 5.8 install that includes Engine/Source",
                self.cfg.engine_root.display()
            )));
        }

        let name = input.class_name.trim();
        let entry = index.get(name).cloned().or_else(|| {
            // Accept a bare name and resolve it through the suggestion pass.
            index
                .suggest(name, 1)
                .first()
                .and_then(|n| index.get(n).cloned())
        });

        let Some(entry) = entry else {
            return Ok(Json(LookupClassOutput::NotFound {
                found: false,
                searched: name.to_string(),
                suggestions: index.suggest(name, 10),
            }));
        };

        let summary = tokio::task::spawn_blocking({
            let entry = entry.clone();
            let name = index
                .symbols
                .iter()
                .find(|(_, v)| v.header == entry.header && v.line == entry.line)
                .map(|(k, _)| k.clone())
                .unwrap_or_else(|| name.to_string());
            move || {
                header_parser::parse(
                    &entry.header,
                    &name,
                    &entry.kind,
                    &entry.module,
                    entry.line,
                    MAX_MEMBERS,
                )
            }
        })
        .await
        .map_err(|e| error::internal(format!("header parse task failed: {e}")))?
        .map_err(|e| error::internal(format!("cannot read {}: {e}", entry.header.display())))?;

        Ok(Json(LookupClassOutput::Found(Box::new(summary))))
    }

    #[tool(
        description = "Regex-search the installed engine's C++ headers for API declarations — the ground truth for signatures, enum values and specifiers. Returns file, line and the matching source line."
    )]
    async fn search_api(
        &self,
        Parameters(input): Parameters<SearchApiInput>,
    ) -> Result<Json<SearchApiOutput>, ErrorData> {
        let max_results = input.max_results.unwrap_or(30).clamp(1, 200);
        let engine_root = self.cfg.engine_root.clone();
        let options = search::SearchOptions {
            max_results,
            module: input.module,
            path_contains: input.path_contains,
        };
        let pattern = input.pattern.clone();

        let (hits, total) =
            tokio::task::spawn_blocking(move || search::search(&engine_root, &pattern, &options))
                .await
                .map_err(|e| error::internal(format!("search task failed: {e}")))?
                .map_err(error::invalid)?;

        Ok(Json(SearchApiOutput {
            truncated: hits.len() < total,
            total,
            hits,
        }))
    }
}
