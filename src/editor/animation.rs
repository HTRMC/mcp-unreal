//! Animation assets, notifies, skeletons, skeletal meshes and physics assets.
//!
//! Variants serialise straight into the plugin request body, so the field
//! names here are the contract with the `McpLinkAnimation` route files.

use rmcp::handler::server::wrapper::{Json, Parameters};
use rmcp::{ErrorData, tool, tool_router};
use serde_json::Value;

use crate::UnrealMcp;

#[derive(serde::Deserialize, serde::Serialize, schemars::JsonSchema)]
#[serde(tag = "operation", rename_all = "snake_case")]
#[schemars(transform = crate::schema::object_with_oneof)]
pub enum AnimAssetOp {
    /// The asset kinds `create` accepts.
    ListKinds {},
    /// New animation asset.
    Create {
        /// "montage", "composite", "blend_space", "blend_space_1d",
        /// "aim_offset" or "aim_offset_1d".
        kind: String,
        /// e.g. /Game/Anims/AM_Attack.
        path: String,
        /// Skeleton to bind to. Optional when `animation` is given.
        skeleton: Option<String>,
        /// Seed the asset with this Anim Sequence; its skeleton is used when
        /// `skeleton` is omitted.
        animation: Option<String>,
        /// Montage only: the slot to create (default "DefaultSlot").
        slot: Option<String>,
    },
    /// Structure of the asset: montage slots, segments and sections; blend
    /// space axes and samples; composite segments.
    Info {
        asset: String,
    },
    /// Montage only: add an animation slot.
    AddSlot {
        asset: String,
        slot: String,
    },
    /// Montage only: a named section at a time on the montage timeline.
    AddSection {
        asset: String,
        name: String,
        time: Option<f64>,
        /// Section to jump to when this one ends (its own name loops it).
        next_section: Option<String>,
    },
    SetSection {
        asset: String,
        name: String,
        time: Option<f64>,
        next_section: Option<String>,
    },
    RemoveSection {
        asset: String,
        name: String,
    },
    /// Place an animation in a montage slot or a composite's track.
    AddSegment {
        asset: String,
        /// Montage only; defaults to the first slot.
        slot: Option<String>,
        animation: String,
        /// Where the segment starts on the montage timeline.
        start_position: Option<f64>,
        /// Trim: which part of the source animation to play.
        anim_start: Option<f64>,
        anim_end: Option<f64>,
        play_rate: Option<f64>,
        loop_count: Option<i32>,
    },
    RemoveSegment {
        asset: String,
        slot: Option<String>,
        /// Segment index from `info`.
        index: i32,
    },
    /// Blend space only: place an animation at a point in the parameter space.
    AddSample {
        asset: String,
        animation: String,
        /// [x, y, z]; x is the first axis. Must lie inside the axis ranges.
        position: [f64; 3],
    },
    SetSample {
        asset: String,
        index: i32,
        position: [f64; 3],
    },
    RemoveSample {
        asset: String,
        index: i32,
    },
    /// Blend space only: name and range of a parameter axis.
    SetAxis {
        asset: String,
        /// 0 = horizontal, 1 = vertical.
        axis: i32,
        name: Option<String>,
        min: Option<f64>,
        max: Option<f64>,
        grid_divisions: Option<i32>,
    },
    Save {
        asset: String,
    },
    /// The Animation Modifier classes available (engine library and loaded
    /// Blueprint modifiers) with their default properties.
    ListModifiers,
    /// The modifiers applied to an Animation Sequence.
    ListAppliedModifiers {
        asset: String,
    },
    /// Add a modifier instance to an Animation Sequence, set its properties
    /// and apply it — distance curves, motion extraction, footstep events,
    /// root re-orientation and the rest.
    ApplyModifier {
        asset: String,
        /// Class name or path from list_modifiers.
        modifier: String,
        /// The modifier's UPROPERTY values.
        properties: Option<serde_json::Value>,
    },
    /// Undo what an applied modifier did (by class name or index).
    RevertModifier {
        asset: String,
        modifier: Option<String>,
        index: Option<i32>,
    },
    /// Revert and drop the modifier instance.
    RemoveModifier {
        asset: String,
        modifier: Option<String>,
        index: Option<i32>,
    },
}

#[derive(serde::Deserialize, serde::Serialize, schemars::JsonSchema)]
#[serde(tag = "operation", rename_all = "snake_case")]
#[schemars(transform = crate::schema::object_with_oneof)]
pub enum AnimNotifyOp {
    /// Notify tracks, notifies, sync markers and float curves on the asset.
    List {
        animation: String,
    },
    AddTrack {
        animation: String,
        track: String,
    },
    RemoveTrack {
        animation: String,
        track: String,
    },
    /// Add a notify or notify state. With `class` it instantiates that notify
    /// class (a duration makes it a state); with only `name` it adds a bare
    /// named notify, which an Anim Blueprint receives as AnimNotify_<Name>.
    AddNotify {
        animation: String,
        /// e.g. "AnimNotify_PlaySound" or a Blueprint notify class path.
        class: Option<String>,
        /// Required when `class` is omitted.
        name: Option<String>,
        time: f64,
        /// Non-zero makes a notify *state* spanning this long.
        duration: Option<f64>,
        /// Notify track name, from `list` (default "1").
        track: Option<String>,
    },
    RemoveNotifies {
        animation: String,
        name: String,
    },
    /// Anim Sequence only.
    AddSyncMarker {
        animation: String,
        name: String,
        time: f64,
        track: Option<String>,
    },
    RemoveSyncMarkers {
        animation: String,
    },
    AddCurve {
        animation: String,
        name: String,
    },
    RemoveCurve {
        animation: String,
        name: String,
    },
    AddCurveKeys {
        animation: String,
        name: String,
        /// [{"time": 0.0, "value": 1.0}, ...].
        keys: Vec<Value>,
    },
    GetCurveKeys {
        animation: String,
        name: String,
    },
}

#[derive(serde::Deserialize, serde::Serialize, schemars::JsonSchema)]
#[serde(tag = "operation", rename_all = "snake_case")]
#[schemars(transform = crate::schema::object_with_oneof)]
pub enum SkeletonOp {
    /// Bones with parents, sockets, virtual bones and animation slot groups.
    Info {
        /// Skeleton path, or a Skeletal Mesh whose skeleton to use.
        skeleton: String,
        max_bones: Option<i32>,
    },
    /// Sockets on the skeleton are shared by every mesh bound to it.
    AddSocket {
        skeleton: String,
        name: String,
        bone: String,
        location: Option<[f64; 3]>,
        /// [Pitch, Yaw, Roll].
        rotation: Option<[f64; 3]>,
        scale: Option<[f64; 3]>,
    },
    RemoveSocket {
        skeleton: String,
        name: String,
    },
    /// A bone driven by two existing bones, usable as an animation target.
    AddVirtualBone {
        skeleton: String,
        source: String,
        target: String,
        name: Option<String>,
    },
    RemoveVirtualBone {
        skeleton: String,
        name: String,
    },
    /// Register a montage slot name.
    AddSlot {
        skeleton: String,
        name: String,
    },
    RemoveSlot {
        skeleton: String,
        name: String,
    },
    AddSlotGroup {
        skeleton: String,
        name: String,
    },
    SetSlotGroup {
        skeleton: String,
        name: String,
        group: String,
    },
    SetPreviewMesh {
        skeleton: String,
        mesh: String,
    },
    Save {
        skeleton: String,
    },
}

#[derive(serde::Deserialize, serde::Serialize, schemars::JsonSchema)]
pub struct BoneWeightEntry {
    /// Bone name.
    pub bone: String,
    /// 0-1; the set is normalized.
    pub weight: f64,
}

#[derive(serde::Deserialize, serde::Serialize, schemars::JsonSchema)]
pub struct VertexSkinWeights {
    /// Source vertex id.
    pub vertex: u32,
    pub bones: Vec<BoneWeightEntry>,
}

#[derive(serde::Deserialize, serde::Serialize, schemars::JsonSchema)]
pub struct MorphDelta {
    /// Render vertex index in the LOD.
    pub vertex: u32,
    /// Position offset [x, y, z] in mesh space.
    pub delta: [f64; 3],
    /// Normal offset [x, y, z] (default none).
    #[serde(skip_serializing_if = "Option::is_none")]
    pub normal: Option<[f64; 3]>,
}

#[derive(serde::Deserialize, serde::Serialize, schemars::JsonSchema)]
#[serde(tag = "operation", rename_all = "snake_case")]
#[schemars(transform = crate::schema::object_with_oneof)]
pub enum PoseAssetOp {
    /// New Pose Asset: from an animation (one pose per frame, named by
    /// `pose_names` or after the frames) or empty on a skeleton.
    Create {
        /// e.g. /Game/Anim/PA_Face.
        path: String,
        /// Skeleton, skeletal mesh or animation path; implied by `animation`.
        skeleton: Option<String>,
        animation: Option<String>,
        pose_names: Option<Vec<String>>,
        /// Also add the skeleton's reference pose under this name.
        reference_pose: Option<String>,
    },
    Info {
        pose_asset: String,
        /// Default 200.
        max_names: Option<i32>,
    },
    /// Re-extract every pose from an animation.
    UpdateFromAnimation {
        pose_asset: String,
        animation: String,
    },
    AddReferencePose {
        pose_asset: String,
        name: String,
    },
    RenamePose {
        pose_asset: String,
        name: String,
        new_name: String,
    },
    DeletePoses {
        pose_asset: String,
        names: Vec<String>,
    },
    DeleteCurves {
        pose_asset: String,
        names: Vec<String>,
    },
    /// Switch between full and additive poses; additive ones are relative to
    /// `base_pose` (or the reference pose with base_pose_index -1).
    SetAdditive {
        pose_asset: String,
        /// Default true.
        additive: Option<bool>,
        base_pose: Option<String>,
        base_pose_index: Option<i32>,
    },
    Save {
        pose_asset: String,
    },
}

#[derive(serde::Deserialize, serde::Serialize, schemars::JsonSchema)]
pub struct MirrorExpression {
    /// e.g. "Left", "_l", "^l_".
    pub find: String,
    /// e.g. "Right", "_r", "r_".
    pub replace: String,
    /// "Prefix" (default), "Suffix" or "RegularExpression".
    #[serde(skip_serializing_if = "Option::is_none")]
    pub method: Option<String>,
}

#[derive(serde::Deserialize, serde::Serialize, schemars::JsonSchema)]
#[serde(tag = "operation", rename_all = "snake_case")]
#[schemars(transform = crate::schema::object_with_oneof)]
pub enum MirrorTableOp {
    /// New Mirror Data Table for a skeleton, filled by running the find /
    /// replace expressions (the project's defaults unless given) over every
    /// bone, notify, curve and sync marker.
    Create {
        /// e.g. /Game/Anim/MDT_Hero.
        path: String,
        /// Skeleton, skeletal mesh or animation path.
        skeleton: String,
        /// Mirror axis "X" (default), "Y" or "Z".
        axis: Option<String>,
        expressions: Option<Vec<MirrorExpression>>,
    },
    Info {
        table: String,
        /// Default 200.
        max_rows: Option<i32>,
    },
    /// Re-run the expressions: "sync" (default, rebuild every row),
    /// "add_missing" (keep edits, add new names) or "update_existing".
    Sync {
        table: String,
        mode: Option<String>,
    },
    SetAxis {
        table: String,
        axis: String,
    },
    /// Replace the expression list, and by default sync the rows from it.
    SetExpressions {
        table: String,
        expressions: Vec<MirrorExpression>,
        sync: Option<bool>,
    },
    Save {
        table: String,
    },
}

#[derive(serde::Deserialize, serde::Serialize, schemars::JsonSchema)]
#[serde(tag = "operation", rename_all = "snake_case")]
#[schemars(transform = crate::schema::object_with_oneof)]
pub enum SkeletalMeshOp {
    /// LOD count, material slots, mesh-only sockets, morph targets and the
    /// assigned physics asset.
    Info {
        mesh: String,
        max_morph_targets: Option<i32>,
    },
    /// Rebuild the LOD chain through the real reduction module.
    RegenerateLod {
        mesh: String,
        /// Total LODs to end up with.
        lod_count: Option<i32>,
    },
    RemoveLods {
        mesh: String,
        /// LOD indices to drop; LOD 0 cannot be removed.
        lods: Vec<i32>,
    },
    SetMaterial {
        mesh: String,
        /// Material slot index from `info`.
        index: i32,
        material: Option<String>,
        /// Rename the slot.
        slot: Option<String>,
    },
    /// A socket on this mesh alone, rather than on the shared skeleton.
    AddSocket {
        mesh: String,
        name: String,
        bone: String,
        location: Option<[f64; 3]>,
        rotation: Option<[f64; 3]>,
        scale: Option<[f64; 3]>,
    },
    RemoveSocket {
        mesh: String,
        name: String,
    },
    RenameSocket {
        mesh: String,
        name: String,
        new_name: String,
    },
    AssignPhysicsAsset {
        mesh: String,
        physics_asset: String,
    },
    Save {
        mesh: String,
    },
    /// A morph target's per-LOD delta counts and a sample of its deltas.
    MorphTargetInfo {
        mesh: String,
        name: String,
        /// LOD to sample deltas from (default 0).
        lod: Option<i32>,
        /// Default 64.
        max_deltas: Option<i32>,
    },
    /// Add a morph target from vertex deltas over one LOD's render vertices,
    /// or replace an existing one's deltas with `replace`.
    AddMorphTarget {
        mesh: String,
        name: String,
        lod: Option<i32>,
        deltas: Vec<MorphDelta>,
        replace: Option<bool>,
        /// Keep deltas whose only change is the normal (default false).
        compare_normal: Option<bool>,
    },
    RemoveMorphTarget {
        mesh: String,
        name: String,
    },
    /// The mesh's clothing assets and the sections each is bound to.
    ListClothing {
        mesh: String,
    },
    /// Build a clothing asset from a section's triangles (the Skeletal Mesh
    /// Editor's "Create Clothing Data from Section"); bind_clothing applies it.
    CreateClothing {
        mesh: String,
        section: i32,
        lod: Option<i32>,
        name: Option<String>,
        /// Remove the source section from the render mesh (default false).
        remove_section: Option<bool>,
    },
    /// Bind a clothing asset (name or guid) to a section so it simulates.
    BindClothing {
        mesh: String,
        clothing: String,
        section: i32,
        lod: Option<i32>,
        /// The clothing asset's own LOD to use (default 0).
        asset_lod: Option<i32>,
    },
    UnbindClothing {
        mesh: String,
        section: i32,
        lod: Option<i32>,
    },
    /// Unbind everywhere and delete the clothing asset.
    RemoveClothing {
        mesh: String,
        clothing: String,
    },
    /// Skin weights of source vertices (mesh description ids): every bone
    /// and weight, for a list of vertices or the first `max_vertices`.
    GetSkinWeights {
        mesh: String,
        lod: Option<i32>,
        vertices: Option<Vec<i32>>,
        /// Default 64.
        max_vertices: Option<i32>,
        /// A skin weight profile; omit for the default weights.
        profile: Option<String>,
    },
    /// Write skin weights for source vertices: bones by name with weights
    /// that are normalized to one; the render data is rebuilt.
    SetSkinWeights {
        mesh: String,
        lod: Option<i32>,
        weights: Vec<VertexSkinWeights>,
        /// A profile to write (created if missing); omit for the default.
        profile: Option<String>,
    },
}

#[derive(serde::Deserialize, serde::Serialize, schemars::JsonSchema)]
#[serde(tag = "operation", rename_all = "snake_case")]
#[schemars(transform = crate::schema::object_with_oneof)]
pub enum PhysicsAssetOp {
    /// Build a physics asset from a skeletal mesh, generating one body per
    /// bone big enough to matter plus the constraints between them — the same
    /// pass the Physics Asset editor runs on a new asset.
    Create {
        /// e.g. /Game/Characters/PHYS_Hero.
        path: String,
        mesh: String,
        /// Bones thinner than this get no body (default 20).
        min_bone_size: Option<f64>,
        /// Start with collision disabled between all bodies.
        disable_collisions: Option<bool>,
        /// Assign the new asset to the mesh (default true).
        assign: Option<bool>,
    },
    /// Bodies with their primitive counts, and constraints with their bones.
    /// Body and constraint settings are properties on the reported paths.
    Info {
        /// Physics Asset path, or a Skeletal Mesh whose asset to use.
        asset: String,
    },
    AddBody {
        asset: String,
        bone: String,
    },
    /// Removing a body also drops the constraints that referenced it.
    RemoveBody {
        asset: String,
        bone: Option<String>,
        index: Option<i32>,
    },
    AddConstraint {
        asset: String,
        /// The constrained (child) bone.
        bone1: String,
        /// The parent bone.
        bone2: String,
        name: Option<String>,
    },
    RemoveConstraint {
        asset: String,
        index: i32,
    },
    Save {
        asset: String,
    },
}

#[tool_router(router = animation_router, vis = "pub(crate)")]
impl UnrealMcp {
    #[tool(
        description = "Author animation assets: create Montages, Anim Composites, Blend Spaces (1D/2D) and Aim Offsets; add montage slots and named sections with their next-section links; place animation segments in a slot or composite track with trim, play rate and loop count; add, move and remove blend space samples and set the parameter axes. Read the whole structure back with info. Save when done."
    )]
    async fn anim_asset_ops(
        &self,
        Parameters(op): Parameters<AnimAssetOp>,
    ) -> Result<Json<Value>, ErrorData> {
        let body =
            serde_json::to_value(op).map_err(|e| ErrorData::internal_error(e.to_string(), None))?;
        self.call_plugin("/api/anim/assets", body).await.map(Json)
    }

    #[tool(
        description = "Notifies, notify tracks, sync markers and float curves on Anim Sequences and Montages, through the engine's own UAnimationBlueprintLibrary. Add a notify by class (a duration makes it a notify state) or as a bare name an Anim Blueprint receives as AnimNotify_<Name>. Notify settings are properties on the reported path, so set_property tunes them."
    )]
    async fn anim_notify_ops(
        &self,
        Parameters(op): Parameters<AnimNotifyOp>,
    ) -> Result<Json<Value>, ErrorData> {
        let body =
            serde_json::to_value(op).map_err(|e| ErrorData::internal_error(e.to_string(), None))?;
        self.call_plugin("/api/anim/notifies", body).await.map(Json)
    }

    #[tool(
        description = "Skeletons: read the bone hierarchy, sockets, virtual bones and animation slot groups; add and remove sockets (shared by every mesh on the skeleton) and virtual bones; register montage slot names and slot groups; set the preview mesh. Pass a Skeletal Mesh path anywhere a skeleton is wanted and its skeleton is used."
    )]
    async fn skeleton_ops(
        &self,
        Parameters(op): Parameters<SkeletonOp>,
    ) -> Result<Json<Value>, ErrorData> {
        let body =
            serde_json::to_value(op).map_err(|e| ErrorData::internal_error(e.to_string(), None))?;
        self.call_plugin("/api/anim/skeleton", body).await.map(Json)
    }

    #[tool(
        description = "Skeletal Meshes: LOD chain (regenerate through the real reduction module, or remove LODs), material slots, mesh-only sockets, morph target listing and physics asset assignment. static_mesh_ops is the equivalent for Static Meshes."
    )]
    async fn skeletal_mesh_ops(
        &self,
        Parameters(op): Parameters<SkeletalMeshOp>,
    ) -> Result<Json<Value>, ErrorData> {
        let body =
            serde_json::to_value(op).map_err(|e| ErrorData::internal_error(e.to_string(), None))?;
        self.call_plugin("/api/anim/mesh", body).await.map(Json)
    }

    #[tool(
        description = "Pose Assets: create one from an animation (a pose per frame) or a skeleton, list its poses, curves and tracks, re-extract from an animation, add the reference pose, rename or delete poses and curves, and switch between full and additive space."
    )]
    async fn pose_asset_ops(
        &self,
        Parameters(op): Parameters<PoseAssetOp>,
    ) -> Result<Json<Value>, ErrorData> {
        let body =
            serde_json::to_value(op).map_err(|e| ErrorData::internal_error(e.to_string(), None))?;
        self.call_plugin("/api/anim/pose_asset", body)
            .await
            .map(Json)
    }

    #[tool(
        description = "Mirror Data Tables: create one for a skeleton from find/replace expressions (Left/Right, _l/_r, …), read its rows and sync status, re-sync after skeleton or expression changes, set the mirror axis or expressions. Rows are DataTable rows, so data_table_ops edits them."
    )]
    async fn mirror_table_ops(
        &self,
        Parameters(op): Parameters<MirrorTableOp>,
    ) -> Result<Json<Value>, ErrorData> {
        let body =
            serde_json::to_value(op).map_err(|e| ErrorData::internal_error(e.to_string(), None))?;
        self.call_plugin("/api/anim/mirror_table", body)
            .await
            .map(Json)
    }

    #[tool(
        description = "Physics Assets: generate a ragdoll from a skeletal mesh (one body per significant bone plus the constraints between them, the same pass the Physics Asset editor runs), then add or remove individual bodies and constraints. Body collision/mass/damping and constraint limits, drives and motion types are ordinary properties on the reported paths, so set_property tunes them."
    )]
    async fn physics_asset_ops(
        &self,
        Parameters(op): Parameters<PhysicsAssetOp>,
    ) -> Result<Json<Value>, ErrorData> {
        let body =
            serde_json::to_value(op).map_err(|e| ErrorData::internal_error(e.to_string(), None))?;
        self.call_plugin("/api/anim/physics", body).await.map(Json)
    }
}
