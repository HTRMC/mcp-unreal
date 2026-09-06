//! IK Rig and IK Retargeter interop (requires the McpLinkIKRig plugin).
//!
//! Variants serialise straight into the plugin request body, so field names
//! here are the contract with `McpIkRigRoutes.cpp`.

use rmcp::handler::server::wrapper::{Json, Parameters};
use rmcp::{ErrorData, tool, tool_router};
use serde_json::Value;

use crate::UnrealMcp;

#[derive(serde::Deserialize, serde::Serialize, schemars::JsonSchema)]
#[serde(tag = "operation", rename_all = "snake_case")]
#[schemars(transform = crate::schema::object_with_oneof)]
pub enum IkRigOp {
    /// The solver types an IK Rig can carry (Full Body IK, Limb, Pole, …).
    ListSolverTypes,
    /// New IK Rig for a skeletal mesh; `auto_retarget` characterizes a
    /// humanoid skeleton (retarget root and chains) as the editor's button does.
    CreateRig {
        /// e.g. /Game/Rigs/IK_Hero.
        path: String,
        skeletal_mesh: String,
        auto_retarget: Option<bool>,
    },
    /// Skeleton bones, solvers, goals, retarget root and chains.
    RigInfo {
        rig: String,
        /// Bones to list (default 200).
        max_bones: Option<i32>,
    },
    /// Generate the retarget root and chains for a recognised humanoid.
    AutoRetarget {
        rig: String,
    },
    AddSolver {
        rig: String,
        /// A type from list_solver_types, e.g. "IKRigFullBodyIKSolver".
        solver: String,
        root_bone: Option<String>,
        end_bone: Option<String>,
    },
    SetSolverEnabled {
        rig: String,
        /// Solver index from rig_info.
        solver: i32,
        enabled: Option<bool>,
    },
    /// An IK goal on a bone, optionally connected to a solver by index.
    AddGoal {
        rig: String,
        name: String,
        bone: String,
        solver: Option<i32>,
    },
    SetRetargetRoot {
        rig: String,
        bone: String,
    },
    /// A retarget chain from a start bone down to an end bone.
    AddChain {
        rig: String,
        name: String,
        start_bone: String,
        end_bone: String,
        goal: Option<String>,
    },
    RemoveChain {
        rig: String,
        name: String,
    },
    SaveRig {
        rig: String,
    },
    /// New IK Retargeter from a source rig to a target rig, chains mapped
    /// by closest name.
    CreateRetargeter {
        /// e.g. /Game/Rigs/RTG_AToB.
        path: String,
        source_rig: String,
        target_rig: String,
    },
    /// Rigs, meshes, poses and the chain mapping.
    RetargeterInfo {
        retargeter: String,
    },
    /// Map a target chain to a source chain (empty source clears it).
    MapChain {
        retargeter: String,
        target_chain: String,
        source_chain: Option<String>,
    },
    /// Re-map every chain: "fuzzy" (default), "exact" or "clear".
    AutoMap {
        retargeter: String,
        method: Option<String>,
    },
    /// Align the retarget pose of the "target" (default) or "source" to the
    /// other side, chain by chain.
    AutoAlign {
        retargeter: String,
        which: Option<String>,
    },
    /// Duplicate animations and bake them onto the target skeleton — the
    /// Retarget Animations window's batch. Copies land next to the sources
    /// or in `target_folder`, renamed by prefix / suffix / search-replace
    /// (default suffix "_Retargeted").
    RetargetAnimations {
        retargeter: String,
        /// Animation Sequence, Montage or Blend Space paths.
        animations: Vec<String>,
        target_folder: Option<String>,
        prefix: Option<String>,
        suffix: Option<String>,
        search: Option<String>,
        replace: Option<String>,
        /// Meshes to retarget between when the rigs have none.
        source_mesh: Option<String>,
        target_mesh: Option<String>,
        include_referenced: Option<bool>,
        overwrite: Option<bool>,
        retain_additive: Option<bool>,
    },
    SaveRetargeter {
        retargeter: String,
    },
}

#[tool_router(router = ik_rig_router, vis = "pub(crate)")]
impl UnrealMcp {
    #[tool(
        description = "IK Rig and IK Retargeter (needs McpLinkIKRig): create a rig for a skeletal mesh, characterize it or add solvers, goals, the retarget root and chains by hand; create a retargeter between two rigs, map chains and align poses; and batch-retarget animations from one skeleton to another."
    )]
    async fn ik_rig_ops(
        &self,
        Parameters(op): Parameters<IkRigOp>,
    ) -> Result<Json<Value>, ErrorData> {
        let body =
            serde_json::to_value(op).map_err(|e| ErrorData::internal_error(e.to_string(), None))?;
        self.call_plugin_with_timeout(
            "/api/anim/ik_rig",
            body,
            std::time::Duration::from_secs(600),
        )
        .await
        .map(Json)
    }
}
