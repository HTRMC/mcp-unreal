//! Class → header index over the installed engine source.
//!
//! ~49k headers is too many to grep per lookup, but far too few to justify a
//! full-text engine like tantivy. We do one parallel scan for reflected type
//! declarations (UCLASS/USTRUCT/UENUM/UINTERFACE), cache the resulting symbol
//! map on disk keyed by engine version (installed source never changes), and
//! parse an individual header on demand when a class is actually looked up.

use std::collections::HashMap;
use std::path::{Path, PathBuf};
use std::sync::Mutex;
use std::sync::atomic::{AtomicUsize, Ordering};

use ignore::{WalkBuilder, WalkState};
use regex::Regex;
use serde::{Deserialize, Serialize};

/// One reflected type declaration found in a header.
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct SymbolEntry {
    /// "class", "struct", "enum", or "interface".
    pub kind: String,
    /// Header file containing the declaration.
    pub header: PathBuf,
    /// Owning module, e.g. "Engine", "UnrealEd", "EnhancedInput".
    pub module: String,
    /// Immediate base type, when the declaration names one.
    pub parent: Option<String>,
    /// 1-based line of the declaration.
    pub line: usize,
}

#[derive(Debug, Serialize, Deserialize)]
pub struct ClassIndex {
    pub engine_version: String,
    pub symbols: HashMap<String, SymbolEntry>,
    pub headers_scanned: usize,
}

fn declaration_re() -> &'static Regex {
    static RE: std::sync::OnceLock<Regex> = std::sync::OnceLock::new();
    RE.get_or_init(|| {
        // `class ENGINE_API ACharacter : public APawn`, `struct FHitResult`,
        // `enum class EInputEvent : uint8`. The *_API macro is optional.
        Regex::new(
            r"^\s*(class|struct|enum)\s+(?:class\s+)?(?:[A-Z][A-Z0-9_]*_API\s+)?([A-Za-z_]\w*)\s*(?::\s*(?:public\s+)?([A-Za-z_][\w:]*))?",
        )
        .unwrap()
    })
}

fn macro_re() -> &'static Regex {
    static RE: std::sync::OnceLock<Regex> = std::sync::OnceLock::new();
    RE.get_or_init(|| Regex::new(r"^\s*(UCLASS|USTRUCT|UENUM|UINTERFACE)\s*\(").unwrap())
}

/// Module name = the directory under `Source/` that owns the header, e.g.
/// `.../Source/Runtime/Engine/Classes/GameFramework/Actor.h` -> "Engine".
pub fn module_of(path: &Path) -> String {
    let mut components = path.components().peekable();
    let mut previous_was_source = false;
    let mut last_dir = String::new();
    while let Some(component) = components.next() {
        let part = component.as_os_str().to_string_lossy();
        if previous_was_source {
            // Engine/Source/<Group>/<Module> for engine code, and
            // <Plugin>/Source/<Module> for plugins.
            let group = part.as_ref();
            if matches!(group, "Runtime" | "Editor" | "Developer" | "Programs") {
                if let Some(next) = components.peek() {
                    return next.as_os_str().to_string_lossy().into_owned();
                }
            }
            return group.to_string();
        }
        previous_was_source = part == "Source";
        if !part.ends_with(".h") {
            last_dir = part.into_owned();
        }
    }
    last_dir
}

fn scan_header(path: &Path, out: &mut Vec<(String, SymbolEntry)>) {
    let Ok(text) = std::fs::read_to_string(path) else {
        return;
    };
    let lines: Vec<&str> = text.lines().collect();
    let module = module_of(path);

    for (i, line) in lines.iter().enumerate() {
        let Some(caps) = macro_re().captures(line) else {
            continue;
        };
        let macro_name = caps.get(1).map_or("", |m| m.as_str());
        // The declaration usually follows the macro, but multi-line specifier
        // lists and comments can sit between them.
        for probe in lines.iter().skip(i + 1).take(24) {
            let trimmed = probe.trim_start();
            if trimmed.is_empty() || trimmed.starts_with("//") || trimmed.starts_with('*') {
                continue;
            }
            if let Some(decl) = declaration_re().captures(probe) {
                let name = decl.get(2).map_or("", |m| m.as_str()).to_string();
                if name.is_empty() {
                    break;
                }
                let kind = match macro_name {
                    "USTRUCT" => "struct",
                    "UENUM" => "enum",
                    "UINTERFACE" => "interface",
                    _ => "class",
                };
                out.push((
                    name,
                    SymbolEntry {
                        kind: kind.to_string(),
                        header: path.to_path_buf(),
                        module: module.clone(),
                        parent: decl.get(3).map(|m| m.as_str().to_string()),
                        line: i + 1,
                    },
                ));
                break;
            }
            // Anything else that isn't a continuation of the macro args means
            // this macro had no declaration after it.
            if !trimmed.starts_with('(') && !trimmed.ends_with(',') && trimmed.contains(';') {
                break;
            }
        }
    }
}

impl ClassIndex {
    /// Scan the engine source tree. Roots are `Engine/Source/{Runtime,Editor,Developer}`
    /// plus `Engine/Plugins`; ThirdParty and generated files are skipped.
    pub fn build(engine_root: &Path, engine_version: String) -> Self {
        let engine = engine_root.join("Engine");
        let roots = [
            engine.join("Source").join("Runtime"),
            engine.join("Source").join("Editor"),
            engine.join("Source").join("Developer"),
            engine.join("Plugins"),
        ];

        let collected: Mutex<Vec<(String, SymbolEntry)>> = Mutex::new(Vec::new());
        let scanned = AtomicUsize::new(0);

        let existing: Vec<&PathBuf> = roots.iter().filter(|p| p.exists()).collect();
        if existing.is_empty() {
            tracing::warn!(
                "no engine source found under {} — lookup_class/search_api will be empty",
                engine.display()
            );
            return Self {
                engine_version,
                symbols: HashMap::new(),
                headers_scanned: 0,
            };
        }

        let mut builder = WalkBuilder::new(existing[0]);
        for root in &existing[1..] {
            builder.add(root);
        }
        builder
            .standard_filters(false)
            .threads(std::thread::available_parallelism().map_or(4, |n| n.get()));

        builder.build_parallel().run(|| {
            let collected = &collected;
            let scanned = &scanned;
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
                let mut found = Vec::new();
                scan_header(path, &mut found);
                scanned.fetch_add(1, Ordering::Relaxed);
                if !found.is_empty() {
                    collected.lock().unwrap().extend(found);
                }
                WalkState::Continue
            })
        });

        let mut symbols: HashMap<String, SymbolEntry> = HashMap::new();
        for (name, entry) in collected.into_inner().unwrap() {
            // Prefer Runtime declarations when a name appears more than once.
            match symbols.get(&name) {
                Some(existing) if !prefer(&entry, existing) => {}
                _ => {
                    symbols.insert(name, entry);
                }
            }
        }

        Self {
            engine_version,
            headers_scanned: scanned.load(Ordering::Relaxed),
            symbols,
        }
    }

    pub fn get(&self, name: &str) -> Option<&SymbolEntry> {
        self.symbols.get(name)
    }

    /// Candidate names for a miss: prefix-insensitive and substring matches.
    pub fn suggest(&self, name: &str, limit: usize) -> Vec<String> {
        let needle = name
            .trim_start_matches(['A', 'U', 'F', 'E', 'I'])
            .to_ascii_lowercase();
        let lower = name.to_ascii_lowercase();
        let mut hits: Vec<&String> = self
            .symbols
            .keys()
            .filter(|k| {
                let kl = k.to_ascii_lowercase();
                kl == lower
                    || kl.trim_start_matches(['a', 'u', 'f', 'e', 'i']) == needle
                    || (needle.len() >= 4 && kl.contains(&needle))
            })
            .collect();
        hits.sort_by_key(|k| (k.len(), (*k).clone()));
        hits.into_iter().take(limit).cloned().collect()
    }

    pub fn load(path: &Path, engine_version: &str) -> Option<Self> {
        let text = std::fs::read_to_string(path).ok()?;
        let index: Self = serde_json::from_str(&text).ok()?;
        if index.engine_version != engine_version || index.symbols.is_empty() {
            return None;
        }
        Some(index)
    }

    pub fn save(&self, path: &Path) {
        if let Some(dir) = path.parent() {
            let _ = std::fs::create_dir_all(dir);
        }
        match serde_json::to_string(self) {
            Ok(json) => {
                if let Err(e) = std::fs::write(path, json) {
                    tracing::warn!("could not cache the class index at {}: {e}", path.display());
                }
            }
            Err(e) => tracing::warn!("could not serialize the class index: {e}"),
        }
    }
}

/// Runtime beats Editor/Developer beats plugins; shorter paths win ties.
fn prefer(candidate: &SymbolEntry, existing: &SymbolEntry) -> bool {
    fn rank(entry: &SymbolEntry) -> u8 {
        let p = entry.header.to_string_lossy();
        if p.contains("Source\\Runtime") || p.contains("Source/Runtime") {
            0
        } else if p.contains("Source\\Editor") || p.contains("Source/Editor") {
            1
        } else if p.contains("Source\\Developer") || p.contains("Source/Developer") {
            2
        } else {
            3
        }
    }
    let (a, b) = (rank(candidate), rank(existing));
    if a != b {
        return a < b;
    }
    candidate.header.as_os_str().len() < existing.header.as_os_str().len()
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn scans_declarations_with_and_without_api_macro() {
        let dir = tempfile::tempdir().unwrap();
        let src = dir.path().join("Source").join("Engine");
        std::fs::create_dir_all(&src).unwrap();
        let header = src.join("Thing.h");
        std::fs::write(
            &header,
            "#pragma once\n\
             UCLASS(config=Game, BlueprintType)\n\
             class ENGINE_API AThing : public AActor\n{\n};\n\
             USTRUCT(BlueprintType)\n\
             struct FThingData\n{\n};\n\
             UENUM()\n\
             enum class EThingMode : uint8\n{\n};\n",
        )
        .unwrap();

        let mut found = Vec::new();
        scan_header(&header, &mut found);
        let by_name: HashMap<_, _> = found.into_iter().collect();

        let thing = by_name.get("AThing").expect("AThing indexed");
        assert_eq!(thing.kind, "class");
        assert_eq!(thing.parent.as_deref(), Some("AActor"));
        assert_eq!(thing.module, "Engine");
        assert_eq!(by_name.get("FThingData").unwrap().kind, "struct");
        assert_eq!(by_name.get("EThingMode").unwrap().kind, "enum");
    }

    #[test]
    fn suggest_matches_ignoring_prefix_letter() {
        let mut symbols = HashMap::new();
        symbols.insert(
            "ACharacter".to_string(),
            SymbolEntry {
                kind: "class".into(),
                header: PathBuf::from("Character.h"),
                module: "Engine".into(),
                parent: Some("APawn".into()),
                line: 1,
            },
        );
        let index = ClassIndex {
            engine_version: "5.8.2".into(),
            symbols,
            headers_scanned: 1,
        };
        assert_eq!(
            index.suggest("Character", 5),
            vec!["ACharacter".to_string()]
        );
        assert_eq!(
            index.suggest("character", 5),
            vec!["ACharacter".to_string()]
        );
    }
}
