//! Schema helpers for operation-based mega-tools.
//!
//! serde's internally-tagged enums give us per-operation required-field
//! validation for free, but schemars renders them as a bare top-level `oneOf`.
//! MCP clients expect an object-typed `inputSchema`, so we inject
//! `"type": "object"` alongside the `oneOf` (valid JSON Schema — the two
//! constrain the same instance).
//!
//! The transform also drops the `null` alternative schemars adds for every
//! `Option<T>` field: optional means "may be omitted", which `required`
//! already expresses, and the tool list is sent to the model on every
//! session, so each byte is paid for repeatedly.

use schemars::Schema;
use serde_json::{Map, Value};

pub fn object_with_oneof(schema: &mut Schema) {
    if let Some(obj) = schema.as_object_mut() {
        obj.entry("type").or_insert_with(|| "object".into());
        strip_null_types_in_map(obj);
    }
}

fn strip_null_types_in_map(map: &mut Map<String, Value>) {
    if let Some(Value::Array(types)) = map.get_mut("type") {
        types.retain(|t| t != "null");
        if types.len() == 1 {
            let only = types.remove(0);
            map.insert("type".to_string(), only);
        }
    }
    for child in map.values_mut() {
        strip_null_types(child);
    }
}

fn strip_null_types(value: &mut Value) {
    match value {
        Value::Object(map) => strip_null_types_in_map(map),
        Value::Array(items) => {
            for item in items {
                strip_null_types(item);
            }
        }
        _ => {}
    }
}
