//! Control Rig interop (requires the McpLinkControlRig plugin).
//!
//! Variants serialise straight into the plugin request body, so field names
//! here are the contract with `McpControlRigRoutes.cpp`.

use rmcp::handler::server::wrapper::{Json, Parameters};
use rmcp::{ErrorData, tool, tool_router};
use serde_json::Value;

use crate::UnrealMcp;

#[derive(serde::Deserialize, serde::Serialize, schemars::JsonSchema)]
pub struct RigTransform {
    #[serde(skip_serializing_if = "Option::is_none")]
    pub location: Option<[f64; 3]>,
    /// Pitch, yaw, roll in degrees.
    #[serde(skip_serializing_if = "Option::is_none")]
    pub rotation: Option<[f64; 3]>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub scale: Option<[f64; 3]>,
}

#[derive(serde::Deserialize, serde::Serialize, schemars::JsonSchema)]
#[serde(tag = "operation", rename_all = "snake_case")]
#[schemars(transform = crate::schema::object_with_oneof)]
pub enum ControlRigOp {
    /// New Control Rig Blueprint, with the bones of a skeleton (or skeletal
    /// mesh) imported when one is given.
    Create {
        /// e.g. /Game/Rigs/CR_Hero.
        path: String,
        /// Skeleton or skeletal mesh path.
        skeleton: Option<String>,
    },
    /// Bones, nulls and controls with parents, types and transforms.
    Info {
        rig: String,
        /// Default 500.
        max_elements: Option<i32>,
    },
    AddBone {
        rig: String,
        name: String,
        parent: Option<String>,
        /// Global transform.
        transform: Option<RigTransform>,
    },
    AddNull {
        rig: String,
        name: String,
        parent: Option<String>,
        transform: Option<RigTransform>,
    },
    /// Add a control: `type` is Bool, Float, Integer, Vector2D, Position,
    /// Scale, Rotator, Transform, TransformNoScale, EulerTransform or
    /// ScaleFloat; `value` its initial value in that type's shape (number,
    /// bool, [x, y], [x, y, z] or a transform object).
    AddControl {
        rig: String,
        name: String,
        #[serde(rename = "type")]
        control_type: String,
        parent: Option<String>,
        value: Option<Value>,
        /// Offset from the parent.
        offset: Option<RigTransform>,
        display_name: Option<String>,
        /// Shape asset name, e.g. "Circle_Thick", "Box_Thin".
        shape: Option<String>,
        shape_visible: Option<bool>,
    },
    /// Remove a bone, null or control (`type` disambiguates a shared name).
    RemoveElement {
        rig: String,
        name: String,
        #[serde(rename = "type")]
        element_type: Option<String>,
    },
    /// Set a control's current (or initial) value on the rig asset.
    SetControlValue {
        rig: String,
        control: String,
        value: Value,
        initial: Option<bool>,
    },
    /// Compile the rig's Blueprint.
    Compile {
        rig: String,
    },
    Save {
        rig: String,
    },
    /// Add a Control Rig track for a rig to a sequence binding (by name or
    /// guid) or to an actor (bound if needed), with its first section.
    AddTrack {
        sequence: String,
        rig: String,
        binding: Option<String>,
        actor: Option<String>,
        name: Option<String>,
    },
    /// The sequence's Control Rig tracks with their rigs and controls.
    ListTracks {
        sequence: String,
    },
    /// Key a control on a track's section at a time (seconds) or display
    /// frame; `value` in the control type's shape.
    SetControlKey {
        sequence: String,
        /// Binding name or guid, or the track name.
        track: String,
        control: String,
        value: Value,
        time: Option<f64>,
        frame: Option<i32>,
    },
}

#[tool_router(router = control_rig_router, vis = "pub(crate)")]
impl UnrealMcp {
    #[tool(
        description = "Control Rig (needs McpLinkControlRig): create a rig Blueprint from a skeleton, author its hierarchy (bones, nulls, controls of every type with shapes and initial values), compile it, and drive it from Sequencer — add a Control Rig track to a binding and key its controls."
    )]
    async fn control_rig_ops(
        &self,
        Parameters(op): Parameters<ControlRigOp>,
    ) -> Result<Json<Value>, ErrorData> {
        let body =
            serde_json::to_value(op).map_err(|e| ErrorData::internal_error(e.to_string(), None))?;
        self.call_plugin("/api/anim/control_rig", body)
            .await
            .map(Json)
    }
}
