//! Asset discovery tools backed by the editor's asset registry.

use rmcp::handler::server::wrapper::{Json, Parameters};
use rmcp::{ErrorData, tool, tool_router};
use serde_json::{Value, json};

use crate::UnrealMcp;

#[derive(serde::Deserialize, serde::Serialize, schemars::JsonSchema)]
#[serde(tag = "operation", rename_all = "snake_case")]
#[schemars(transform = crate::schema::object_with_oneof)]
pub enum AssetOp {
    /// Make an empty asset of any class that has a "create new" factory —
    /// Data Assets, Curve assets, Curve Tables, String Tables, User-Defined
    /// Structs and Enums, Materials, Blueprints, and so on. Types that only
    /// exist by import (meshes, textures, audio) need `import` instead.
    Create {
        /// Full asset path, e.g. /Game/Data/DA_Weapon.
        path: String,
        /// Asset class: "CurveFloat", "DataTable", "UserDefinedEnum",
        /// "/Script/Engine.DataAsset", ...
        class: String,
        /// Properties to set on the factory before it runs, e.g.
        /// {"ParentClass": "/Script/Engine.Actor"} for a Blueprint or
        /// {"DataAssetClass": "/Game/BP_Item.BP_Item_C"} for a Data Asset.
        factory_properties: Option<Value>,
    },
    /// Import source files from disk — FBX, glTF, OBJ, images, audio, CSV.
    /// Runs the automated importer, so no dialog appears and defaults apply;
    /// tune the result afterwards with set_property on the asset's
    /// AssetImportData, then `reimport`.
    Import {
        /// Absolute paths of the source files.
        files: Vec<String>,
        /// Content folder to import into, e.g. /Game/Meshes.
        destination: String,
        /// Overwrite assets that already exist (default true).
        replace_existing: Option<bool>,
        /// Force a specific factory class instead of letting the extension
        /// pick, e.g. "InterchangeImportTestPlanFactory".
        factory: Option<String>,
        /// Properties to set on that factory first.
        factory_properties: Option<Value>,
        /// Import through the Interchange framework (FBX, glTF, OBJ, USD,
        /// images, …) with the project's pipeline stack instead of the legacy
        /// importer. Asynchronous under the hood; the reply waits.
        interchange: Option<bool>,
        /// Interchange pipeline assets to use instead of the project's stack.
        pipelines: Option<Vec<String>>,
    },
    /// Re-run an asset's import from its recorded source file.
    Reimport {
        path: String,
        /// Point the asset at a different source file instead.
        file: Option<String>,
    },
    /// Rename an asset in place, move it to another folder, or both, fixing
    /// up every reference. Leaves a redirector behind — clean those up with
    /// fixup_redirectors.
    Rename {
        path: String,
        /// New asset name, without a path.
        new_name: Option<String>,
        /// New folder, e.g. /Game/Meshes/Rocks.
        destination: Option<String>,
    },
    /// Move assets to another folder, fixing up references.
    Move {
        /// Assets to move.
        paths: Vec<String>,
        /// Destination folder.
        destination: String,
    },
    /// Copy an asset to a new path.
    Duplicate {
        path: String,
        /// Full path of the copy, e.g. /Game/Meshes/SM_Rock_B.
        destination: String,
    },
    /// Delete assets. Anything still referenced is kept unless `force`.
    Delete {
        paths: Vec<String>,
        /// Null out references from other assets and delete anyway.
        force: Option<bool>,
    },
    /// Write assets to their .uasset files.
    Save { paths: Vec<String> },
    /// Save every dirty package in the project.
    SaveAll {
        /// Include modified maps (default true).
        include_maps: Option<bool>,
    },
    /// Create a content folder.
    CreateFolder { path: String },
    /// Delete a content folder. Refuses a folder that still holds assets
    /// unless `delete_contents`.
    DeleteFolder {
        path: String,
        delete_contents: Option<bool>,
        /// Null out references and delete anyway. Needed whenever the folder's
        /// assets reference each other.
        force: Option<bool>,
    },
    /// Rename or move a content folder and everything under it.
    RenameFolder { path: String, destination: String },
    /// Sub-folders of a content path.
    ListFolders {
        /// Default "/Game".
        path: Option<String>,
        recursive: Option<bool>,
    },
    /// Resolve the redirectors a rename or move left behind, so referencing
    /// assets point straight at the new path.
    FixupRedirectors {
        /// Folder to sweep (default "/Game").
        path: Option<String>,
    },
    /// Export assets to files on disk using their registered exporters.
    Export {
        paths: Vec<String>,
        /// Absolute directory to write into.
        directory: String,
        /// File extension that picks the exporter — "gltf" or "glb" (static and
        /// skeletal meshes, materials, levels), "fbx", "obj", "t3d", "png",
        /// "wav", "copy" … Omit for each asset's first registered exporter.
        /// `list_exporters` shows what an asset's class can be written as.
        format: Option<String>,
    },
    /// The exporter classes registered in this editor — every one, or only
    /// those that take the asset at `path` — with their file extensions.
    ListExporters { path: Option<String> },
    /// An asset's metadata tags.
    GetMetadata { path: String },
    /// Set one metadata tag, or remove it by omitting `value`.
    SetMetadata {
        path: String,
        key: String,
        value: Option<String>,
    },
}

#[derive(serde::Deserialize, schemars::JsonSchema)]
pub struct SearchAssetsInput {
    /// Package path to search under (default "/Game"). Use "/Engine" for engine content.
    pub path_prefix: Option<String>,
    /// Restrict to this asset class and its subclasses, e.g. "StaticMesh", "Material", "Blueprint".
    pub class: Option<String>,
    /// Substring match on asset name.
    pub name_contains: Option<String>,
    /// Max results (default 50, cap 500).
    pub max_results: Option<u32>,
}

#[derive(serde::Deserialize, schemars::JsonSchema)]
pub struct AssetInfoInput {
    /// Asset path, e.g. "/Game/Meshes/SM_Rock" (the .AssetName suffix is optional).
    pub path: String,
}

#[tool_router(router = asset_router, vis = "pub(crate)")]
impl UnrealMcp {
    #[tool(
        description = "Asset lifecycle: create empty assets of any factory-backed class, import and reimport source files from disk (FBX, glTF, OBJ, images, audio, CSV), rename, move, duplicate and delete assets with reference fixup, save individual assets or every dirty package, manage content folders, fix up redirectors, export assets to disk, and read or write metadata tags. Use search_assets and get_asset_info to find what to operate on."
    )]
    async fn asset_ops(
        &self,
        Parameters(op): Parameters<AssetOp>,
    ) -> Result<Json<Value>, ErrorData> {
        let body =
            serde_json::to_value(op).map_err(|e| ErrorData::internal_error(e.to_string(), None))?;
        self.call_plugin("/api/assets/ops", body).await.map(Json)
    }

    #[tool(
        description = "Search project assets by path, class, and name. Returns name, class, package and object path."
    )]
    async fn search_assets(
        &self,
        Parameters(input): Parameters<SearchAssetsInput>,
    ) -> Result<Json<Value>, ErrorData> {
        self.call_plugin(
            "/api/assets/search",
            json!({
                "path_prefix": input.path_prefix,
                "class": input.class,
                "name_contains": input.name_contains,
                "max_results": input.max_results,
            }),
        )
        .await
        .map(Json)
    }

    #[tool(description = "Get an asset's metadata plus its dependencies and referencers.")]
    async fn get_asset_info(
        &self,
        Parameters(input): Parameters<AssetInfoInput>,
    ) -> Result<Json<Value>, ErrorData> {
        self.call_plugin("/api/assets/info", json!({"path": input.path}))
            .await
            .map(Json)
    }
}
