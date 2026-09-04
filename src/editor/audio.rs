//! Sound Cue authoring.
//!
//! Variants serialise straight into the plugin request body, so the field
//! names here are the contract with `McpAudioRoutes.cpp`.

use rmcp::handler::server::wrapper::{Json, Parameters};
use rmcp::{ErrorData, tool, tool_router};
use serde_json::Value;

use crate::UnrealMcp;

#[derive(serde::Deserialize, serde::Serialize, schemars::JsonSchema)]
#[serde(tag = "operation", rename_all = "snake_case")]
#[schemars(transform = crate::schema::object_with_oneof)]
pub enum SoundCueOp {
    /// New Sound Cue asset.
    Create {
        /// e.g. /Game/Audio/Cue_Footstep.
        path: String,
        /// Wire a WavePlayer for this Sound Wave straight away.
        sound_wave: Option<String>,
    },
    /// Every SoundNode class this project has, with how many children each
    /// takes. `class` values from here are what add_node accepts.
    ListNodeClasses {
        name_contains: Option<String>,
    },
    /// The cue's nodes, their children and which one is the output. Node
    /// settings live on the reported `path` — set_property edits them.
    GetGraph {
        cue: String,
    },
    /// Add a node. With no `parent` it becomes the cue's output node.
    AddNode {
        cue: String,
        /// e.g. "SoundNodeWavePlayer", "SoundNodeRandom", "SoundNodeMixer".
        class: String,
        /// Node id from get_graph.
        parent: Option<String>,
        /// Child slot on the parent; appended when omitted.
        index: Option<i32>,
        /// For a SoundNodeWavePlayer: the Sound Wave to play.
        sound_wave: Option<String>,
    },
    /// Wire an existing node in as a child of another.
    Connect {
        cue: String,
        parent: String,
        child: String,
        index: Option<i32>,
    },
    /// Make a node the cue's output.
    SetRoot {
        cue: String,
        node: String,
    },
    RemoveNode {
        cue: String,
        node: String,
    },
    Save {
        cue: String,
    },
}

#[tool_router(router = audio_router, vis = "pub(crate)")]
impl UnrealMcp {
    #[tool(
        description = "Author Sound Cues: create the asset, discover the SoundNode classes available, add nodes (wave players, random, mixer, modulator, attenuation, concatenator, ...), wire them into a tree, choose the output node, and save. Node settings are properties on the reported node path, so set_property tunes them. Sound Classes, Submixes, Attenuation and Concurrency assets need no special tool — asset_ops create plus set_property covers those."
    )]
    async fn sound_cue_ops(
        &self,
        Parameters(op): Parameters<SoundCueOp>,
    ) -> Result<Json<Value>, ErrorData> {
        let body =
            serde_json::to_value(op).map_err(|e| ErrorData::internal_error(e.to_string(), None))?;
        self.call_plugin("/api/audio/cue", body).await.map(Json)
    }
}
