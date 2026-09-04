use rmcp::ErrorData;

/// Error helpers that name the fix, not just the failure.
pub fn internal(msg: impl Into<String>) -> ErrorData {
    ErrorData::internal_error(msg.into(), None)
}

pub fn invalid(msg: impl Into<String>) -> ErrorData {
    ErrorData::invalid_params(msg.into(), None)
}

pub fn no_project() -> ErrorData {
    invalid(
        "no UE project configured — set MCP_UNREAL_PROJECT to a .uproject file (or its folder), \
         or run the server from inside a UE project directory",
    )
}

pub fn no_editor_binary(path: &std::path::Path) -> ErrorData {
    invalid(format!(
        "UnrealEditor-Cmd not found at {} — set UE_ENGINE_ROOT to your UE 5.8 install \
         (the folder containing Engine/, e.g. C:\\Program Files\\Epic Games\\UE_5.8) \
         or UE_EDITOR_PATH to the binary itself",
        path.display()
    ))
}

pub fn missing_tool(path: &std::path::Path, what: &str) -> ErrorData {
    invalid(format!(
        "{what} not found at {} — set UE_ENGINE_ROOT to your UE 5.8 install",
        path.display()
    ))
}

pub fn editor_offline(port: u16, source: impl std::fmt::Display) -> ErrorData {
    internal(format!(
        "editor unreachable at http://127.0.0.1:{port} — ensure the Unreal Editor is running \
         with the McpLink plugin enabled (source error: {source})"
    ))
}
