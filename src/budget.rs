//! Token-budget helpers: every tool that can return large output goes through
//! these caps so responses stay readable in an LLM context window.

/// Last `n` lines of `s`, joined with `\n`.
pub fn last_n_lines(s: &str, n: usize) -> String {
    let lines: Vec<&str> = s.lines().collect();
    let start = lines.len().saturating_sub(n);
    lines[start..].join("\n")
}

/// Cap a vec, returning the kept items and the total count before capping.
pub fn cap<T>(mut v: Vec<T>, max: usize) -> (Vec<T>, usize) {
    let total = v.len();
    v.truncate(max);
    (v, total)
}

/// Truncate to `max` chars on a char boundary, appending an ellipsis marker.
pub fn truncate_chars(s: &str, max: usize) -> String {
    if s.chars().count() <= max {
        return s.to_string();
    }
    let cut: String = s.chars().take(max).collect();
    format!("{cut}… (truncated)")
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn last_n_lines_shorter_input() {
        assert_eq!(last_n_lines("a\nb", 5), "a\nb");
        assert_eq!(last_n_lines("a\nb\nc", 2), "b\nc");
    }

    #[test]
    fn cap_reports_total() {
        let (kept, total) = cap(vec![1, 2, 3, 4], 2);
        assert_eq!(kept, vec![1, 2]);
        assert_eq!(total, 4);
    }

    #[test]
    fn truncate_marks() {
        assert_eq!(truncate_chars("hello", 10), "hello");
        assert!(truncate_chars("hello world", 5).ends_with("(truncated)"));
    }
}
