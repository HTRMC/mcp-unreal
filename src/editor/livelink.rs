//! Live Link interop (requires the McpLinkLiveLink plugin).
//!
//! Variants serialise straight into the plugin request body, so field names
//! here are the contract with `McpLiveLinkRoutes.cpp`.

use rmcp::handler::server::wrapper::{Json, Parameters};
use rmcp::{ErrorData, tool, tool_router};
use serde_json::Value;

use crate::UnrealMcp;

#[derive(serde::Deserialize, serde::Serialize, schemars::JsonSchema)]
#[serde(tag = "operation", rename_all = "snake_case")]
#[schemars(transform = crate::schema::object_with_oneof)]
pub enum LiveLinkOp {
    /// The client's sources (id, type, status, machine) and subjects (name,
    /// source, role, enabled).
    Status,
    /// Source factories that can be added, subject roles and virtual subject
    /// classes.
    ListSourceTypes,
    /// Add a source through a factory's connection string (what a preset
    /// stores for it) — e.g. "LiveLinkMessageBusSourceFactory" with the
    /// provider's address, or a device plugin's factory.
    AddSource {
        factory: String,
        connection: Option<String>,
    },
    /// A source that only carries virtual subjects.
    AddVirtualSource { name: String },
    RemoveSource {
        /// Source id from status (or its type text when unique).
        source: String,
    },
    /// A virtual subject on a virtual source; class defaults to
    /// LiveLinkAnimationVirtualSubject.
    AddVirtualSubject {
        name: String,
        source: String,
        class: Option<String>,
    },
    RemoveVirtualSubject {
        name: String,
        source: Option<String>,
    },
    SetSubjectEnabled {
        name: String,
        source: Option<String>,
        enabled: Option<bool>,
    },
    /// Capture the client's sources and subjects into a Live Link Preset
    /// asset (created when missing).
    SavePreset {
        /// e.g. /Game/VP/LLP_Stage.
        preset: String,
    },
    /// Add a preset's sources and subjects to the client; `replace` clears
    /// the client first.
    ApplyPreset {
        preset: String,
        replace: Option<bool>,
    },
}

#[tool_router(router = live_link_router, vis = "pub(crate)")]
impl UnrealMcp {
    #[tool(
        description = "Live Link (needs McpLinkLiveLink): list the client's sources and subjects, add sources from a factory's connection string or as virtual-subject containers, add or remove virtual subjects, enable subjects, and save or apply Live Link Preset assets."
    )]
    async fn live_link_ops(
        &self,
        Parameters(op): Parameters<LiveLinkOp>,
    ) -> Result<Json<Value>, ErrorData> {
        let body =
            serde_json::to_value(op).map_err(|e| ErrorData::internal_error(e.to_string(), None))?;
        self.call_plugin("/api/vp/live_link", body).await.map(Json)
    }
}
