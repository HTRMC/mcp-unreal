//! On-demand parse of a single engine header into a structured class summary.
//!
//! Deliberately line-based and best-effort: the goal is to hand an agent the
//! real signatures from its own engine build, not to be a C++ front end.
//! Anything that fails to parse simply doesn't appear.

use std::path::Path;

use regex::Regex;
use serde::Serialize;

#[derive(Debug, Serialize, schemars::JsonSchema)]
pub struct Member {
    /// Reflection specifiers, e.g. "BlueprintCallable, Category=\"Character\"".
    pub specifiers: String,
    /// The declaration line as written in the header.
    pub declaration: String,
    /// Doc comment directly above the declaration, when there is one.
    pub doc: Option<String>,
}

#[derive(Debug, Serialize, schemars::JsonSchema)]
pub struct ClassSummary {
    pub name: String,
    pub kind: String,
    pub module: String,
    pub parent: Option<String>,
    pub header: String,
    /// UCLASS/USTRUCT specifiers.
    pub specifiers: String,
    pub doc: Option<String>,
    pub properties: Vec<Member>,
    pub functions: Vec<Member>,
    /// Total counts before capping, so the caller knows what was elided.
    pub property_count: usize,
    pub function_count: usize,
    pub enum_values: Vec<String>,
}

fn macro_open_re() -> &'static Regex {
    static RE: std::sync::OnceLock<Regex> = std::sync::OnceLock::new();
    RE.get_or_init(|| Regex::new(r"^\s*(UPROPERTY|UFUNCTION)\s*\(").unwrap())
}

/// Collect a doc comment sitting immediately above `index`.
fn doc_above(lines: &[&str], index: usize) -> Option<String> {
    let mut collected: Vec<String> = Vec::new();
    let mut i = index;
    while i > 0 {
        i -= 1;
        let trimmed = lines[i].trim();
        if trimmed.is_empty() {
            if collected.is_empty() {
                continue;
            }
            break;
        }
        if let Some(rest) = trimmed.strip_prefix("///") {
            collected.push(rest.trim().to_string());
        } else if let Some(rest) = trimmed.strip_prefix("//") {
            collected.push(rest.trim().to_string());
        } else if trimmed == "*/" || trimmed.starts_with("/*") || trimmed.starts_with('*') {
            let cleaned = trimmed
                .trim_start_matches("/**")
                .trim_start_matches("/*")
                .trim_start_matches('*')
                .trim_end_matches("*/")
                .trim_end_matches('*')
                .trim_end_matches('/')
                .trim();
            if !cleaned.is_empty() {
                collected.push(cleaned.to_string());
            }
            if trimmed.starts_with("/*") {
                break;
            }
        } else {
            break;
        }
    }
    if collected.is_empty() {
        return None;
    }
    collected.reverse();
    Some(collected.join(" "))
}

/// Read a parenthesised specifier list that may span lines, returning the
/// contents and the index of the line where it closes.
fn read_specifiers(lines: &[&str], start: usize) -> (String, usize) {
    let mut depth: i32 = 0;
    let mut text = String::new();
    let mut started = false;
    for (offset, line) in lines.iter().enumerate().skip(start).take(24) {
        for ch in line.chars() {
            match ch {
                '(' => {
                    depth += 1;
                    started = true;
                    if depth == 1 {
                        continue;
                    }
                }
                ')' => {
                    depth -= 1;
                    if depth == 0 {
                        return (text.trim().to_string(), offset);
                    }
                }
                _ => {}
            }
            if started && depth >= 1 {
                text.push(ch);
            }
        }
        text.push(' ');
    }
    (text.trim().to_string(), start)
}

/// Next code line at or after `start`, skipping blanks, comments and macros.
fn next_declaration(lines: &[&str], start: usize) -> Option<(usize, String)> {
    for (offset, line) in lines.iter().enumerate().skip(start).take(12) {
        let trimmed = line.trim();
        if trimmed.is_empty()
            || trimmed.starts_with("//")
            || trimmed.starts_with('*')
            || trimmed.starts_with("/*")
            || trimmed.starts_with('#')
        {
            continue;
        }
        return Some((offset, trimmed.trim_end_matches(';').trim().to_string()));
    }
    None
}

/// Parse `header` for the declaration of `name`.
///
/// `macro_line` is the 1-based line of the UCLASS/USTRUCT macro as recorded by
/// the index; it disambiguates the real declaration from forward declarations
/// (`class ACharacter;`) earlier in the same header. Pass 0 if unknown.
pub fn parse(
    header: &Path,
    name: &str,
    kind: &str,
    module: &str,
    macro_line: usize,
    max_members: usize,
) -> std::io::Result<ClassSummary> {
    let text = std::fs::read_to_string(header)?;
    let lines: Vec<&str> = text.lines().collect();

    // Locate the declaration of this specific type; a header can hold several.
    let decl_re = Regex::new(&format!(
        r"^\s*(?:class|struct|enum)\s+(?:class\s+)?(?:[A-Z][A-Z0-9_]*_API\s+)?{}\b",
        regex::escape(name)
    ))
    .map_err(|e| std::io::Error::new(std::io::ErrorKind::InvalidInput, e))?;
    // Forward declarations (`class ACharacter;`) are not the definition.
    let is_definition = |l: &&str| decl_re.is_match(l) && !l.trim_end().ends_with(';');
    let decl_line = if macro_line > 0 && macro_line <= lines.len() {
        // The definition follows the macro within a few lines.
        lines
            .iter()
            .enumerate()
            .skip(macro_line - 1)
            .take(24)
            .find(|(_, l)| is_definition(l))
            .map(|(i, _)| i)
            .or_else(|| lines.iter().position(is_definition))
    } else {
        lines.iter().position(is_definition)
    };

    let mut specifiers = String::new();
    let mut doc = None;
    let mut parent = None;
    if let Some(decl_idx) = decl_line {
        // The UCLASS/USTRUCT macro sits just above the declaration.
        for probe in (decl_idx.saturating_sub(24)..decl_idx).rev() {
            let trimmed = lines[probe].trim_start();
            if trimmed.starts_with("UCLASS(")
                || trimmed.starts_with("USTRUCT(")
                || trimmed.starts_with("UENUM(")
                || trimmed.starts_with("UINTERFACE(")
            {
                let (spec, _) = read_specifiers(&lines, probe);
                specifiers = spec;
                doc = doc_above(&lines, probe);
                break;
            }
        }
        if doc.is_none() {
            doc = doc_above(&lines, decl_idx);
        }
        if let Some(caps) = Regex::new(r":\s*public\s+([A-Za-z_][\w:]*)")
            .ok()
            .and_then(|re| re.captures(lines[decl_idx]))
        {
            parent = caps.get(1).map(|m| m.as_str().to_string());
        }
    }

    let mut properties = Vec::new();
    let mut functions = Vec::new();
    let start = decl_line.unwrap_or(0);

    let mut i = start;
    while i < lines.len() {
        let line = lines[i];
        // Stop at the next reflected type in the same header.
        if i > start
            && (line.trim_start().starts_with("UCLASS(")
                || line.trim_start().starts_with("USTRUCT("))
        {
            break;
        }
        if let Some(caps) = macro_open_re().captures(line) {
            let macro_name = caps.get(1).map_or("", |m| m.as_str()).to_string();
            let (spec, close_idx) = read_specifiers(&lines, i);
            if let Some((decl_idx, declaration)) = next_declaration(&lines, close_idx + 1) {
                let member = Member {
                    specifiers: spec,
                    doc: doc_above(&lines, i),
                    declaration,
                };
                if macro_name == "UPROPERTY" {
                    properties.push(member);
                } else {
                    functions.push(member);
                }
                i = decl_idx;
            } else {
                i = close_idx;
            }
        }
        i += 1;
    }

    // Enum values: the body between the declaration's braces.
    let mut enum_values = Vec::new();
    if kind == "enum" {
        if let Some(decl_idx) = decl_line {
            for line in lines.iter().skip(decl_idx + 1).take(200) {
                let trimmed = line.trim();
                if trimmed.starts_with('}') {
                    break;
                }
                if trimmed.is_empty()
                    || trimmed.starts_with("//")
                    || trimmed.starts_with('{')
                    || trimmed.starts_with("UMETA")
                    || trimmed.starts_with("enum")
                    || trimmed.starts_with('*')
                    || trimmed.starts_with("/*")
                {
                    continue;
                }
                let value = trimmed
                    .split("UMETA")
                    .next()
                    .unwrap_or("")
                    .trim()
                    .trim_end_matches(',')
                    .trim();
                if !value.is_empty() && !value.starts_with('#') {
                    enum_values.push(value.to_string());
                }
            }
        }
    }

    let property_count = properties.len();
    let function_count = functions.len();
    properties.truncate(max_members);
    functions.truncate(max_members);

    Ok(ClassSummary {
        name: name.to_string(),
        kind: kind.to_string(),
        module: module.to_string(),
        parent,
        header: header.display().to_string(),
        specifiers,
        doc,
        properties,
        functions,
        property_count,
        function_count,
        enum_values,
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    const HEADER: &str = r#"#pragma once

/** A thing that does something. */
UCLASS(config=Game, BlueprintType,
    meta=(ShortTooltip="A thing."))
class ENGINE_API AThing : public AActor
{
    GENERATED_BODY()
public:
    /// How fast the thing moves.
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Thing")
    float Speed = 100.0f;

    UPROPERTY()
    TObjectPtr<UStaticMeshComponent> Mesh;

    /** Makes the thing go. */
    UFUNCTION(BlueprintCallable, Category="Thing")
    ENGINE_API void Go(float Amount);
};
"#;

    fn write_header() -> (tempfile::TempDir, std::path::PathBuf) {
        let dir = tempfile::tempdir().unwrap();
        let path = dir.path().join("Thing.h");
        std::fs::write(&path, HEADER).unwrap();
        (dir, path)
    }

    #[test]
    fn parses_class_with_multiline_specifiers_and_docs() {
        let (_dir, path) = write_header();
        let summary = parse(&path, "AThing", "class", "Engine", 0, 40).unwrap();

        assert_eq!(summary.parent.as_deref(), Some("AActor"));
        assert!(summary.specifiers.contains("config=Game"));
        assert!(
            summary.specifiers.contains("ShortTooltip"),
            "multi-line specifiers joined"
        );
        assert_eq!(summary.doc.as_deref(), Some("A thing that does something."));

        assert_eq!(summary.property_count, 2);
        assert_eq!(summary.properties[0].declaration, "float Speed = 100.0f");
        assert_eq!(
            summary.properties[0].doc.as_deref(),
            Some("How fast the thing moves.")
        );
        assert!(summary.properties[0].specifiers.contains("EditAnywhere"));

        assert_eq!(summary.function_count, 1);
        assert_eq!(
            summary.functions[0].declaration,
            "ENGINE_API void Go(float Amount)"
        );
        assert_eq!(
            summary.functions[0].doc.as_deref(),
            Some("Makes the thing go.")
        );
    }

    #[test]
    fn caps_members_but_reports_true_counts() {
        let (_dir, path) = write_header();
        let summary = parse(&path, "AThing", "class", "Engine", 0, 1).unwrap();
        assert_eq!(summary.properties.len(), 1);
        assert_eq!(summary.property_count, 2);
    }

    #[test]
    fn single_line_block_comment_has_no_trailing_slash() {
        let dir = tempfile::tempdir().unwrap();
        let path = dir.path().join("Doc.h");
        std::fs::write(
            &path,
            "/** A one-line summary. */
USTRUCT()
struct FDoc
{
};
",
        )
        .unwrap();
        let summary = parse(&path, "FDoc", "struct", "Engine", 0, 40).unwrap();
        assert_eq!(summary.doc.as_deref(), Some("A one-line summary."));
    }

    #[test]
    fn parses_enum_values() {
        let dir = tempfile::tempdir().unwrap();
        let path = dir.path().join("Mode.h");
        std::fs::write(
            &path,
            "UENUM(BlueprintType)\nenum class EMode : uint8\n{\n    Idle,\n    Running UMETA(DisplayName=\"Run\"),\n};\n",
        )
        .unwrap();
        let summary = parse(&path, "EMode", "enum", "Engine", 0, 40).unwrap();
        assert_eq!(
            summary.enum_values,
            vec!["Idle".to_string(), "Running".to_string()],
            "the declaration line itself must not appear as a value"
        );
    }
}
