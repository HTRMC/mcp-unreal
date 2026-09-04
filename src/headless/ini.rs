//! Minimal reader/writer for UE `.ini` config files.
//!
//! UE config is not standard INI: a key may legitimately appear many times in
//! one section, and lines carry `+ - . !` prefixes meaning add / remove /
//! add-unique / clear. We keep the file's exact lines and edit in place, so
//! comments, formatting and duplicate keys all survive a write.

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Entry {
    /// Prefix character (`+`, `-`, `.`, `!`) or empty for a plain assignment.
    pub prefix: String,
    pub key: String,
    pub value: String,
}

/// Parse `text` into `(section, entries)` pairs, in file order.
pub fn sections(text: &str) -> Vec<(String, Vec<Entry>)> {
    let mut out: Vec<(String, Vec<Entry>)> = Vec::new();
    let mut current: Option<(String, Vec<Entry>)> = None;

    for line in text.lines() {
        let trimmed = line.trim();
        if trimmed.starts_with('[') && trimmed.ends_with(']') {
            if let Some(section) = current.take() {
                out.push(section);
            }
            current = Some((trimmed[1..trimmed.len() - 1].to_string(), Vec::new()));
            continue;
        }
        if trimmed.is_empty() || trimmed.starts_with(';') || trimmed.starts_with("//") {
            continue;
        }
        let Some((lhs, value)) = trimmed.split_once('=') else {
            continue;
        };
        let (prefix, key) = split_prefix(lhs.trim());
        if let Some((_, entries)) = current.as_mut() {
            entries.push(Entry {
                prefix: prefix.to_string(),
                key: key.to_string(),
                value: value.trim().to_string(),
            });
        }
    }
    if let Some(section) = current {
        out.push(section);
    }
    out
}

fn split_prefix(lhs: &str) -> (&str, &str) {
    match lhs.chars().next() {
        Some(c @ ('+' | '-' | '.' | '!')) => (&lhs[..c.len_utf8()], lhs[c.len_utf8()..].trim()),
        _ => ("", lhs),
    }
}

/// All values for `key` in `section` (several for array-style keys).
pub fn get(text: &str, section: &str, key: &str) -> Vec<String> {
    sections(text)
        .into_iter()
        .filter(|(name, _)| name == section)
        .flat_map(|(_, entries)| entries)
        .filter(|e| e.key.eq_ignore_ascii_case(key))
        .map(|e| e.value)
        .collect()
}

/// Set `key` to `value` in `section`, replacing the first plain assignment or
/// appending. Creates the section when missing. Returns the new file text.
pub fn set(text: &str, section: &str, key: &str, value: &str) -> String {
    // `str::lines` strips any carriage return; `join` restores the original
    // line ending so a CRLF config stays CRLF.
    let mut lines: Vec<String> = text.lines().map(str::to_string).collect();
    let mut in_section = false;
    let mut section_start: Option<usize> = None;
    let mut section_end = lines.len();

    for (i, line) in lines.iter().enumerate() {
        let trimmed = line.trim();
        if trimmed.starts_with('[') && trimmed.ends_with(']') {
            let name = &trimmed[1..trimmed.len() - 1];
            if in_section {
                section_end = i;
                break;
            }
            if name == section {
                in_section = true;
                section_start = Some(i);
            }
            continue;
        }
        if !in_section {
            continue;
        }
        if let Some((lhs, _)) = trimmed.split_once('=') {
            let (prefix, existing) = split_prefix(lhs.trim());
            if prefix.is_empty() && existing.eq_ignore_ascii_case(key) {
                lines[i] = format!("{key}={value}");
                return join(&lines, text);
            }
        }
    }

    match section_start {
        Some(_) => {
            // Insert at the end of the existing section, before trailing blanks.
            let mut insert_at = section_end.min(lines.len());
            while insert_at > 0 && lines[insert_at - 1].trim().is_empty() {
                insert_at -= 1;
            }
            lines.insert(insert_at, format!("{key}={value}"));
        }
        None => {
            if !lines.is_empty() && !lines.last().is_some_and(|l| l.trim().is_empty()) {
                lines.push(String::new());
            }
            lines.push(format!("[{section}]"));
            lines.push(format!("{key}={value}"));
        }
    }
    join(&lines, text)
}

/// Remove every entry for `key` in `section`. Returns the new text and how many
/// lines were removed.
pub fn remove(text: &str, section: &str, key: &str) -> (String, usize) {
    let mut out: Vec<String> = Vec::new();
    let mut in_section = false;
    let mut removed = 0;

    for line in text.lines() {
        let trimmed = line.trim();
        if trimmed.starts_with('[') && trimmed.ends_with(']') {
            in_section = &trimmed[1..trimmed.len() - 1] == section;
            out.push(line.to_string());
            continue;
        }
        if in_section {
            if let Some((lhs, _)) = trimmed.split_once('=') {
                let (_, existing) = split_prefix(lhs.trim());
                if existing.eq_ignore_ascii_case(key) {
                    removed += 1;
                    continue;
                }
            }
        }
        out.push(line.to_string());
    }
    (join(&out, text), removed)
}

/// Rejoin using the original file's line ending, so rewriting a CRLF config on
/// Windows doesn't turn the entire file into a diff.
fn join(lines: &[String], original: &str) -> String {
    let newline = if original.contains("\r\n") {
        "\r\n"
    } else {
        "\n"
    };
    let mut text = lines.join(newline);
    if original.ends_with('\n') {
        text.push_str(newline);
    }
    text
}

#[cfg(test)]
mod tests {
    use super::*;

    const SAMPLE: &str = "[/Script/Engine.RendererSettings]\n\
                          r.DefaultFeature.AutoExposure=False\n\
                          ; a comment\n\
                          +r.Custom=1\n\
                          +r.Custom=2\n\
                          \n\
                          [/Script/EngineSettings.GameMapsSettings]\n\
                          GameDefaultMap=/Game/Maps/Old\n";

    #[test]
    fn reads_repeated_and_prefixed_keys() {
        assert_eq!(
            get(SAMPLE, "/Script/Engine.RendererSettings", "r.Custom"),
            vec!["1".to_string(), "2".to_string()]
        );
        assert_eq!(
            get(
                SAMPLE,
                "/Script/EngineSettings.GameMapsSettings",
                "GameDefaultMap"
            ),
            vec!["/Game/Maps/Old".to_string()]
        );
        assert!(get(SAMPLE, "/Script/Engine.RendererSettings", "Missing").is_empty());
    }

    #[test]
    fn set_replaces_existing_and_keeps_other_lines() {
        let updated = set(
            SAMPLE,
            "/Script/EngineSettings.GameMapsSettings",
            "GameDefaultMap",
            "/Game/Maps/New",
        );
        assert_eq!(
            get(
                &updated,
                "/Script/EngineSettings.GameMapsSettings",
                "GameDefaultMap"
            ),
            vec!["/Game/Maps/New".to_string()]
        );
        assert!(updated.contains("; a comment"), "comments survive");
        assert!(updated.contains("+r.Custom=1"), "array entries survive");
    }

    #[test]
    fn set_appends_within_existing_section() {
        let updated = set(SAMPLE, "/Script/Engine.RendererSettings", "r.New", "7");
        let renderer_index = updated.find("[/Script/Engine.RendererSettings]").unwrap();
        let maps_index = updated
            .find("[/Script/EngineSettings.GameMapsSettings]")
            .unwrap();
        let new_index = updated.find("r.New=7").unwrap();
        assert!(new_index > renderer_index && new_index < maps_index);
    }

    #[test]
    fn set_creates_missing_section() {
        let updated = set(SAMPLE, "NewSection", "Key", "Value");
        assert!(updated.contains("[NewSection]\nKey=Value"));
    }

    #[test]
    fn preserves_crlf_line_endings() {
        let crlf = SAMPLE.replace('\n', "\r\n");
        let updated = set(&crlf, "/Script/Engine.RendererSettings", "r.New", "7");
        assert!(updated.contains("r.New=7"));
        // Every LF must still be part of a CRLF pair.
        assert_eq!(
            updated.matches("\r\n").count(),
            updated.matches('\n').count(),
            "rewriting a CRLF config must not convert it to LF"
        );
    }

    #[test]
    fn remove_drops_all_matching_entries() {
        let (updated, removed) = remove(SAMPLE, "/Script/Engine.RendererSettings", "r.Custom");
        assert_eq!(removed, 2);
        assert!(get(&updated, "/Script/Engine.RendererSettings", "r.Custom").is_empty());
        assert!(updated.contains("r.DefaultFeature.AutoExposure=False"));
    }
}
