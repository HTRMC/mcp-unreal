//! HTTP client for the McpLink editor plugin (loopback only).
//!
//! Envelope contract: the plugin responds `{"ok": true, "data": ...}` on
//! success and `{"ok": false, "error": {"code": ..., "message": ...}}` on
//! failure, with a real HTTP status code. Legacy bare `{"error": "..."}`
//! bodies are tolerated.

use std::time::Duration;

use serde_json::Value;

use crate::budget::truncate_chars;

const REQUEST_TIMEOUT: Duration = Duration::from_secs(30);
const PING_TIMEOUT: Duration = Duration::from_secs(2);

#[derive(Clone)]
pub struct EditorClient {
    http: reqwest::Client,
    base: String,
    pub port: u16,
}

#[derive(Debug, thiserror::Error)]
pub enum EditorError {
    #[error("{0}")]
    Unreachable(String),
    #[error("plugin error{}: {message}", code.map(|c| format!(" (HTTP {c})")).unwrap_or_default())]
    Api { code: Option<u16>, message: String },
    #[error("invalid plugin response: {0}")]
    BadResponse(String),
}

impl EditorClient {
    pub fn new(port: u16) -> Self {
        let http = reqwest::Client::builder()
            .timeout(REQUEST_TIMEOUT)
            .build()
            .expect("reqwest client");
        Self {
            http,
            base: format!("http://127.0.0.1:{port}"),
            port,
        }
    }

    /// POST a JSON body to a plugin endpoint and unwrap the envelope.
    pub async fn post(&self, path: &str, body: Value) -> Result<Value, EditorError> {
        let url = format!("{}{}", self.base, path);
        let resp = self
            .http
            .post(&url)
            .json(&body)
            .send()
            .await
            .map_err(|e| EditorError::Unreachable(e.to_string()))?;
        Self::unwrap_envelope(resp).await
    }

    /// Quick health probe with a short timeout; used by the `status` tool.
    pub async fn ping(&self) -> Result<Value, EditorError> {
        let url = format!("{}/api/status", self.base);
        let resp = self
            .http
            .get(&url)
            .timeout(PING_TIMEOUT)
            .send()
            .await
            .map_err(|e| EditorError::Unreachable(e.to_string()))?;
        Self::unwrap_envelope(resp).await
    }

    async fn unwrap_envelope(resp: reqwest::Response) -> Result<Value, EditorError> {
        let status = resp.status();
        let text = resp
            .text()
            .await
            .map_err(|e| EditorError::BadResponse(e.to_string()))?;
        let value: Value = serde_json::from_str(&text).map_err(|_| {
            if status.is_success() {
                EditorError::BadResponse(truncate_chars(&text, 200))
            } else {
                EditorError::Api {
                    code: Some(status.as_u16()),
                    message: truncate_chars(&text, 500),
                }
            }
        })?;

        if let Some(obj) = value.as_object() {
            if let Some(ok) = obj.get("ok").and_then(Value::as_bool) {
                if ok {
                    return Ok(obj.get("data").cloned().unwrap_or(value.clone()));
                }
                let (code, message) = match obj.get("error") {
                    Some(Value::Object(e)) => (
                        e.get("code").and_then(Value::as_u64).map(|c| c as u16),
                        e.get("message")
                            .and_then(Value::as_str)
                            .unwrap_or("unknown plugin error")
                            .to_string(),
                    ),
                    Some(Value::String(s)) => (None, s.clone()),
                    _ => (None, "unknown plugin error".to_string()),
                };
                return Err(EditorError::Api {
                    code: code.or(Some(status.as_u16())),
                    message,
                });
            }
            // Legacy convention: top-level "error" string.
            if let Some(err) = obj.get("error").and_then(Value::as_str) {
                if !err.is_empty() {
                    return Err(EditorError::Api {
                        code: Some(status.as_u16()),
                        message: err.to_string(),
                    });
                }
            }
        }

        if !status.is_success() {
            return Err(EditorError::Api {
                code: Some(status.as_u16()),
                message: truncate_chars(&text, 500),
            });
        }
        Ok(value)
    }
}

impl EditorError {
    /// Convert into an MCP error with remediation guidance.
    pub fn into_tool_error(self, port: u16) -> rmcp::ErrorData {
        match self {
            EditorError::Unreachable(src) => crate::error::editor_offline(port, src),
            other => crate::error::internal(other.to_string()),
        }
    }
}
