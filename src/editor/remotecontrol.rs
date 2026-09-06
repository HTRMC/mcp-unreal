//! Remote Control preset interop (requires the McpLinkRemoteControl plugin).
//!
//! Variants serialise straight into the plugin request body, so field names
//! here are the contract with `McpRemoteControlRoutes.cpp`.

use rmcp::handler::server::wrapper::{Json, Parameters};
use rmcp::{ErrorData, tool, tool_router};
use serde_json::Value;

use crate::UnrealMcp;

#[derive(serde::Deserialize, serde::Serialize, schemars::JsonSchema)]
#[serde(tag = "operation", rename_all = "snake_case")]
#[schemars(transform = crate::schema::object_with_oneof)]
pub enum RemoteControlOp {
    /// New, empty Remote Control Preset asset.
    Create {
        /// e.g. /Game/VP/RCP_Stage.
        path: String,
    },
    /// Every exposed property, function and actor with its label and id.
    Info {
        preset: String,
    },
    Save {
        preset: String,
    },
    /// Expose a property of an object (an actor, component or asset path)
    /// under a label; nested paths like "RelativeLocation.X" work.
    ExposeProperty {
        preset: String,
        object: String,
        property: String,
        label: Option<String>,
    },
    /// Expose a callable function of an object.
    ExposeFunction {
        preset: String,
        object: String,
        function: String,
        label: Option<String>,
    },
    /// Expose a whole actor (by path) so clients can reach any of its fields.
    ExposeActor {
        preset: String,
        object: String,
        label: Option<String>,
    },
    /// Remove an exposed entity by label.
    Unexpose {
        preset: String,
        label: String,
    },
    Rename {
        preset: String,
        label: String,
        new_label: String,
    },
}

#[tool_router(router = remote_control_router, vis = "pub(crate)")]
impl UnrealMcp {
    #[tool(
        description = "Remote Control presets (needs McpLinkRemoteControl): create a preset, expose object properties, functions and whole actors under labels, rename or unexpose them, list what a preset exposes and save it — the asset the Remote Control web app and REST API serve."
    )]
    async fn remote_control_ops(
        &self,
        Parameters(op): Parameters<RemoteControlOp>,
    ) -> Result<Json<Value>, ErrorData> {
        let body =
            serde_json::to_value(op).map_err(|e| ErrorData::internal_error(e.to_string(), None))?;
        self.call_plugin("/api/vp/remote_control", body)
            .await
            .map(Json)
    }
}
