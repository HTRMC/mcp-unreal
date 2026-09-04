//! User-Defined Structs and Enums: the members and entries inside them.

use rmcp::handler::server::wrapper::{Json, Parameters};
use rmcp::{ErrorData, tool, tool_router};
use serde_json::Value;

use crate::UnrealMcp;

#[derive(serde::Deserialize, serde::Serialize, schemars::JsonSchema)]
#[serde(tag = "operation", rename_all = "snake_case")]
#[schemars(transform = crate::schema::object_with_oneof)]
pub enum UserTypeOp {
    /// New User-Defined Struct. It starts with one placeholder member.
    CreateStruct {
        /// e.g. /Game/Data/S_Item.
        path: String,
    },
    /// A struct's members with their types and defaults.
    GetStruct {
        #[serde(rename = "struct")]
        struct_path: String,
    },
    /// Add a member.
    AddStructMember {
        #[serde(rename = "struct")]
        struct_path: String,
        name: String,
        /// Same type grammar as blueprint_modify add_variable: bool, int,
        /// float, string, name, text, vector, "struct:<Struct>",
        /// "enum:<Enum>", "object:<Class>", "array<int>", "map<name,float>", ...
        #[serde(rename = "type")]
        member_type: String,
        default: Option<String>,
    },
    /// Remove a member. A struct must keep at least one.
    RemoveStructMember {
        #[serde(rename = "struct")]
        struct_path: String,
        name: String,
    },
    RenameStructMember {
        #[serde(rename = "struct")]
        struct_path: String,
        name: String,
        new_name: String,
    },
    /// Retype a member, fixing up everything that reads it.
    SetStructMemberType {
        #[serde(rename = "struct")]
        struct_path: String,
        name: String,
        #[serde(rename = "type")]
        member_type: String,
    },
    SetStructMemberDefault {
        #[serde(rename = "struct")]
        struct_path: String,
        name: String,
        default: Option<String>,
    },
    /// New User-Defined Enum.
    CreateEnum {
        /// e.g. /Game/Data/E_Team.
        path: String,
    },
    /// An enum's entries with their indices and display names.
    GetEnum {
        #[serde(rename = "enum")]
        enum_path: String,
    },
    /// Append an entry with this display name.
    AddEnumEntry {
        #[serde(rename = "enum")]
        enum_path: String,
        name: String,
    },
    /// Remove an entry by display name or index.
    RemoveEnumEntry {
        #[serde(rename = "enum")]
        enum_path: String,
        name: Option<String>,
        index: Option<i32>,
    },
    RenameEnumEntry {
        #[serde(rename = "enum")]
        enum_path: String,
        name: Option<String>,
        index: Option<i32>,
        new_name: String,
    },
    /// Write the struct or enum asset to disk.
    Save { path: String },
}

#[tool_router(router = user_type_router, vis = "pub(crate)")]
impl UnrealMcp {
    #[tool(
        description = "Author Blueprint (User-Defined) Structs and Enums: create them, then add, rename, retype, default and remove struct members, and add, rename and remove enum entries. Members are addressed by the name the editor shows. Every edit recompiles the type and fixes up what references it. Once a struct exists, blueprint_modify and data_table_ops can use it by path."
    )]
    async fn user_type_ops(
        &self,
        Parameters(op): Parameters<UserTypeOp>,
    ) -> Result<Json<Value>, ErrorData> {
        let body =
            serde_json::to_value(op).map_err(|e| ErrorData::internal_error(e.to_string(), None))?;
        self.call_plugin("/api/types/user_defined", body)
            .await
            .map(Json)
    }
}
