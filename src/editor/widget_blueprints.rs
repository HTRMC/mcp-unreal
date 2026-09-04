//! Widget Blueprint (UMG) authoring: widget trees, layout slots, events.
//!
//! Variants serialise straight into the plugin request body, so field names
//! here are the contract with `McpWidgetBlueprintRoutes.cpp`.

use rmcp::handler::server::wrapper::{Json, Parameters};
use rmcp::{ErrorData, tool, tool_router};
use serde_json::Value;

use crate::UnrealMcp;

#[derive(serde::Deserialize, serde::Serialize, schemars::JsonSchema)]
#[serde(tag = "operation", rename_all = "snake_case")]
#[schemars(transform = crate::schema::object_with_oneof)]
pub enum WidgetBlueprintQuery {
    /// Find Widget Blueprint assets in the project.
    List {
        /// Package path to search under (default "/Game").
        path_prefix: Option<String>,
        /// Max results (default 100, cap 500).
        max_results: Option<u32>,
    },
    /// The widget tree (names, classes, slots with layout values, named-slot
    /// content), variables, graphs, bound events, animations and property
    /// bindings. Widget names from here feed the modify operations.
    Inspect {
        /// Asset path, e.g. "/Game/UI/WBP_Hud".
        blueprint: String,
    },
    /// One widget's editable properties (current values, in the JSON shapes
    /// set_widget accepts), its slot, and the events it can bind.
    GetWidget {
        blueprint: String,
        /// Widget name, label, or object path.
        widget: String,
    },
    /// Placeable widget classes (TextBlock, Button, Image, VerticalBox, ...),
    /// flagging panels and how many children they accept.
    ListWidgetClasses {
        /// Substring filter, e.g. "Box".
        filter: Option<String>,
        /// Default 200.
        max_results: Option<u32>,
    },
    /// Events (with parameters) and editable properties of a widget class.
    WidgetClassInfo {
        /// Class short name ("Button") or Widget Blueprint path.
        class: String,
    },
}

#[derive(serde::Deserialize, serde::Serialize, schemars::JsonSchema)]
#[serde(tag = "operation", rename_all = "snake_case")]
#[schemars(transform = crate::schema::object_with_oneof)]
pub enum WidgetBlueprintModify {
    /// Create a Widget Blueprint (compiled, in memory — finish with
    /// `blueprint_modify save`).
    Create {
        /// Destination package path, e.g. "/Game/UI/WBP_Hud".
        path: String,
        /// UserWidget subclass to derive from (default UserWidget).
        parent_class: Option<String>,
        /// Root panel class (default "CanvasPanel"; "none" for an empty tree).
        root_widget: Option<String>,
    },
    /// Add a widget under a panel (default: the root). Panels that hold a
    /// single child (Border, SizeBox, ScaleBox, ...) refuse a second one.
    AddWidget {
        blueprint: String,
        /// Widget class short name ("TextBlock", "Button", "Image",
        /// "ProgressBar", "VerticalBox", "Overlay", ...) or a Widget Blueprint
        /// path to nest a user widget.
        class: String,
        /// Widget name; defaults to "<Class>_N". Also the Blueprint variable name.
        name: Option<String>,
        /// Parent widget name (must be a panel). Default: root.
        parent: Option<String>,
        /// Child index in the parent (-1 / omitted appends).
        index: Option<i32>,
        /// Expose as a Blueprint variable (engine default depends on the class;
        /// bind_event turns it on when needed).
        is_variable: Option<bool>,
    },
    /// Remove a widget and its descendants (also drops bound events).
    RemoveWidget { blueprint: String, widget: String },
    /// Reparent a widget into another panel, keeping compatible slot settings.
    MoveWidget {
        blueprint: String,
        widget: String,
        /// New parent panel.
        parent: String,
        index: Option<i32>,
    },
    RenameWidget {
        blueprint: String,
        widget: String,
        new_name: String,
    },
    /// Set widget properties from JSON: {"Text": "Play"}, {"Percent": 0.5},
    /// {"ColorAndOpacity": {"SpecifiedColor": {"R":1,"G":0,"B":0,"A":1}}},
    /// {"Brush": {"ResourceObject": "/Game/UI/T_Icon.T_Icon"}}, {"Visibility":
    /// "Collapsed"}. Nested structs update only the fields given.
    SetWidget {
        blueprint: String,
        widget: String,
        properties: serde_json::Map<String, Value>,
    },
    /// Set layout on the widget's slot in its parent. Canvas: {"LayoutData":
    /// {"Offsets": {"Left":40,"Top":20,"Right":300,"Bottom":60}, "Anchors":
    /// {"Minimum":{"X":0,"Y":0},"Maximum":{"X":0,"Y":0}}, "Alignment":
    /// {"X":0,"Y":0}}, "bAutoSize": true, "ZOrder": 1}. Box slots: {"Padding":
    /// {"Left":8,"Top":4,"Right":8,"Bottom":4}, "HorizontalAlignment":
    /// "HAlign_Fill", "VerticalAlignment": "VAlign_Center", "Size":
    /// {"SizeRule": "Fill", "Value": 1}}.
    SetSlot {
        blueprint: String,
        widget: String,
        properties: serde_json::Map<String, Value>,
    },
    /// Expose (or hide) a widget as a Blueprint variable.
    SetVariable {
        blueprint: String,
        widget: String,
        is_variable: bool,
    },
    /// Create an event node for a widget delegate (Button "OnClicked",
    /// "OnPressed", "OnHovered"; CheckBox "OnCheckStateChanged"; Slider
    /// "OnValueChanged"; EditableTextBox "OnTextCommitted", ...) in the event
    /// graph. Returns the node to wire with blueprint_modify.
    BindEvent {
        blueprint: String,
        widget: String,
        event: String,
    },
    /// Wrap a widget in a new panel (e.g. put a TextBlock inside a Border).
    WrapWidget {
        blueprint: String,
        widget: String,
        /// Panel class for the wrapper.
        class: String,
    },
    /// Swap a widget for another class, keeping its name, children (panel to
    /// panel) and compatible properties.
    ReplaceWidget {
        blueprint: String,
        widget: String,
        class: String,
    },
}

#[tool_router(router = widget_blueprint_router, vis = "pub(crate)")]
impl UnrealMcp {
    #[tool(
        description = "Inspect Widget Blueprints (UMG): list assets, dump a Blueprint's widget tree with slot layout, variables, graphs and bound events, read one widget's properties, or discover placeable widget classes and a class's events/properties."
    )]
    async fn widget_blueprint_query(
        &self,
        Parameters(op): Parameters<WidgetBlueprintQuery>,
    ) -> Result<Json<Value>, ErrorData> {
        let body =
            serde_json::to_value(op).map_err(|e| ErrorData::internal_error(e.to_string(), None))?;
        self.call_plugin("/api/widget_blueprints/query", body)
            .await
            .map(Json)
    }

    #[tool(
        description = "Author Widget Blueprints (UMG designer as an API): create one, add/move/remove/rename/wrap/replace widgets in the tree, set widget properties and slot layout from JSON, expose widgets as variables, and bind widget events (OnClicked, ...) to event-graph nodes. Wire event logic with blueprint_modify; compile and save with blueprint_modify. To show it in PIE without game code: call_function on /Script/UMG.Default__WidgetBlueprintLibrary 'Create' (WorldContextObject and OwningPlayer = the PIE PlayerController path, WidgetType = the _C class path), then 'AddToViewport' on the returned widget; ui_query umg reads it back."
    )]
    async fn widget_blueprint_modify(
        &self,
        Parameters(op): Parameters<WidgetBlueprintModify>,
    ) -> Result<Json<Value>, ErrorData> {
        let body =
            serde_json::to_value(op).map_err(|e| ErrorData::internal_error(e.to_string(), None))?;
        self.call_plugin("/api/widget_blueprints/modify", body)
            .await
            .map(Json)
    }
}
