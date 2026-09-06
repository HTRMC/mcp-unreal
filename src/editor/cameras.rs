//! Gameplay Cameras interop (requires the McpLinkCameras plugin).
//!
//! Variants serialise straight into the plugin request body, so field names
//! here are the contract with `McpCameraRoutes.cpp`.

use rmcp::handler::server::wrapper::{Json, Parameters};
use rmcp::{ErrorData, tool, tool_router};
use serde_json::Value;

use crate::UnrealMcp;

#[derive(serde::Deserialize, serde::Serialize, schemars::JsonSchema)]
#[serde(tag = "operation", rename_all = "snake_case")]
#[schemars(transform = crate::schema::object_with_oneof)]
pub enum GameplayCameraOp {
    /// Camera node classes with their categories, child slots and defaults.
    ListNodeTypes {
        filter: Option<String>,
    },
    ListDirectorTypes,
    /// New, empty Camera Rig asset.
    CreateRig {
        /// e.g. /Game/Cameras/CR_ThirdPerson.
        path: String,
    },
    /// The rig's node tree with every node's properties and children.
    RigInfo {
        rig: String,
    },
    /// Add a camera node ("Array" for a sequence, "BoomArm", "Offset",
    /// "FieldOfView", "AttachToPlayerPawn", "InputAxisBinding2D", …) as the
    /// root, or under `parent` (an Array node appends to Children; typed
    /// slots such as InputSlot are picked by class or named with `slot`).
    /// Camera parameters take a literal (75, [0, 0, 300]) or {"Value": …} /
    /// {"Variable": "/Game/…"}.
    AddNode {
        rig: String,
        node: String,
        parent: Option<String>,
        slot: Option<String>,
        name: Option<String>,
        properties: Option<Value>,
    },
    SetNode {
        rig: String,
        node: String,
        properties: Value,
    },
    /// Detach and delete a node (and, through it, its subtree).
    RemoveNode {
        rig: String,
        node: String,
    },
    /// Build the rig headless and report the build log.
    BuildRig {
        rig: String,
    },
    /// New Camera asset with a director (default Single) pointing at a rig.
    CreateCamera {
        /// e.g. /Game/Cameras/CAM_ThirdPerson.
        path: String,
        rig: Option<String>,
        director: Option<String>,
        director_properties: Option<Value>,
    },
    /// Change the director, its rig or its properties.
    SetCamera {
        camera: String,
        rig: Option<String>,
        director: Option<String>,
        director_properties: Option<Value>,
    },
    CameraInfo {
        camera: String,
    },
    /// Build the camera and every rig it references.
    BuildCamera {
        camera: String,
    },
    /// Save a Camera or Camera Rig asset (saving builds it).
    Save {
        asset: String,
    },
    /// Spawn a Gameplay Camera actor running the camera and make it the
    /// player's view target in a playing world.
    Activate {
        camera: String,
        world: Option<String>,
        player_index: Option<i32>,
        location: Option<[f64; 3]>,
        rotation: Option<[f64; 3]>,
        name: Option<String>,
    },
    /// Destroy the camera actor and return the view to the pawn.
    Deactivate {
        actor: String,
        world: Option<String>,
        player_index: Option<i32>,
    },
}

#[tool_router(router = gameplay_camera_router, vis = "pub(crate)")]
impl UnrealMcp {
    #[tool(
        description = "Gameplay Cameras (needs McpLinkCameras): build Camera Rig assets as trees of camera nodes (boom arm, offset, field of view, attach, input bindings, blends…), create Camera assets with a director, build them headless with the build log, save, and activate a camera on the player in PIE."
    )]
    async fn gameplay_camera_ops(
        &self,
        Parameters(op): Parameters<GameplayCameraOp>,
    ) -> Result<Json<Value>, ErrorData> {
        let body =
            serde_json::to_value(op).map_err(|e| ErrorData::internal_error(e.to_string(), None))?;
        self.call_plugin("/api/cameras/gameplay", body)
            .await
            .map(Json)
    }
}
