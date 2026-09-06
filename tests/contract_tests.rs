//! Contract tests: the Rust half of `tests/fixtures/contract/`.
//!
//! Each fixture is one real request/response exchange captured from a live
//! UE 5.8 editor. Two things are checked per fixture:
//!
//! 1. **Request shape** — the fixture's `request` deserializes into the tool's
//!    own input type, and building the plugin body from it reproduces that
//!    request exactly. A field renamed on the Rust side fails here.
//! 2. **Response handling** — wiremock replays the fixture's envelope with its
//!    real HTTP status, and `EditorClient` must unwrap it to exactly `data`, or
//!    to an error carrying the plugin's code and message.
//!
//! The plugin checks the same files from the other side
//! (`McpLink.Core.Contract.Fixtures`), so a rename on either half breaks a test
//! rather than an agent.

use std::path::{Path, PathBuf};

use mcp_unreal::editor::client::{EditorClient, EditorError};
use serde_json::{Map, Value};
use wiremock::matchers::{body_json, method, path};
use wiremock::{Mock, MockServer, ResponseTemplate};

#[derive(serde::Deserialize)]
struct Fixture {
    name: String,
    #[allow(dead_code)]
    description: String,
    route: String,
    tool: String,
    request: Value,
    status: u16,
    response: Value,
}

fn contract_dir() -> PathBuf {
    Path::new(env!("CARGO_MANIFEST_DIR")).join("tests/fixtures/contract")
}

fn load_fixtures() -> Vec<Fixture> {
    let mut fixtures: Vec<Fixture> = std::fs::read_dir(contract_dir())
        .expect("contract fixture directory")
        .filter_map(Result::ok)
        .filter(|e| e.path().extension().is_some_and(|x| x == "json"))
        .map(|e| {
            let text = std::fs::read_to_string(e.path()).expect("read fixture");
            serde_json::from_str(&text)
                .unwrap_or_else(|err| panic!("{}: {err}", e.path().display()))
        })
        .collect();
    fixtures.sort_by(|a, b| a.name.cmp(&b.name));
    assert!(!fixtures.is_empty(), "no contract fixtures found");
    fixtures
}

/// JSON has one number type, but serde_json compares `30` and `30.0` as
/// different values. The plugin reads every number as a double, so the contract
/// is the numeric value, not how it was written.
fn normalize_numbers(value: &Value) -> Value {
    match value {
        Value::Number(n) => n
            .as_f64()
            .and_then(serde_json::Number::from_f64)
            .map_or_else(|| value.clone(), Value::Number),
        Value::Array(items) => Value::Array(items.iter().map(normalize_numbers).collect()),
        Value::Object(map) => Value::Object(
            map.iter()
                .map(|(k, v)| (k.clone(), normalize_numbers(v)))
                .collect(),
        ),
        _ => value.clone(),
    }
}

/// Drop top-level keys whose value is null: `Option::None` fields serialize as
/// null and the plugin treats them as absent, so they are not part of the
/// contract. Nested nulls are left alone — inside a `properties` map a null is
/// a real value.
fn strip_top_level_nulls(value: &Value) -> Value {
    let value = normalize_numbers(value);
    match value.as_object() {
        Some(map) => Value::Object(
            map.iter()
                .filter(|(_, v)| !v.is_null())
                .map(|(k, v)| (k.clone(), v.clone()))
                .collect::<Map<String, Value>>(),
        ),
        None => value,
    }
}

/// Round-trip a fixture request through the tool's own input type and body
/// builder, returning the body the server would actually POST.
fn wire_body(fixture: &Fixture) -> Option<Value> {
    use mcp_unreal::editor::*;
    let req = fixture.request.clone();
    let body = match fixture.tool.as_str() {
        "get_property" => object::get_property_body(&from_json(&fixture.name, req)),
        "set_property" => object::set_property_body(&from_json(&fixture.name, req)),
        "input_inject" => play::input_body(from_json(&fixture.name, req)),
        "pie_control" => play::pie_body(from_json(&fixture.name, req)),
        "capture_viewport" => capture::capture_body(&from_json(&fixture.name, req)),
        "perf_ops" => to_json::<perf::PerfOp>(&fixture.name, req),
        "blueprint_modify" => blueprints::blueprint_modify_body(from_json(&fixture.name, req)),
        "blueprint_debug" => to_json::<blueprints::BlueprintDebugOp>(&fixture.name, req),
        "widget_blueprint_modify" => {
            to_json::<widget_blueprints::WidgetBlueprintModify>(&fixture.name, req)
        }
        "widget_blueprint_query" => {
            to_json::<widget_blueprints::WidgetBlueprintQuery>(&fixture.name, req)
        }
        "anim_blueprint_query" => {
            to_json::<anim_blueprints::AnimBlueprintQuery>(&fixture.name, req)
        }
        "anim_blueprint_modify" => {
            to_json::<anim_blueprints::AnimBlueprintModify>(&fixture.name, req)
        }
        "chaos_ops" => to_json::<chaos::ChaosOp>(&fixture.name, req),
        "visual_log_ops" => to_json::<visuallog::VisualLogOp>(&fixture.name, req),
        "ui_ops" => to_json::<uiops::UiOp>(&fixture.name, req),
        "eqs_ops" => to_json::<eqs::EqsOp>(&fixture.name, req),
        "texture_ops" => to_json::<content::TextureOp>(&fixture.name, req),
        "material_parameter_collection_ops" => {
            to_json::<content::MaterialParameterCollectionOp>(&fixture.name, req)
        }
        "asset_ops" => to_json::<assets::AssetOp>(&fixture.name, req),
        "replay_ops" => to_json::<play::ReplayOp>(&fixture.name, req),
        "geometry_script_ops" => to_json::<geometry::GeometryScriptOp>(&fixture.name, req),
        "control_rig_ops" => to_json::<controlrig::ControlRigOp>(&fixture.name, req),
        "level_snapshot_ops" => to_json::<snapshots::LevelSnapshotOp>(&fixture.name, req),
        "ik_rig_ops" => to_json::<ikrig::IkRigOp>(&fixture.name, req),
        "remote_control_ops" => to_json::<remotecontrol::RemoteControlOp>(&fixture.name, req),
        "live_link_ops" => to_json::<livelink::LiveLinkOp>(&fixture.name, req),
        "pose_search_ops" => to_json::<posesearch::PoseSearchOp>(&fixture.name, req),
        "mass_ops" => to_json::<mass::MassOp>(&fixture.name, req),
        "chooser_ops" => to_json::<chooser::ChooserOp>(&fixture.name, req),
        "gameplay_camera_ops" => to_json::<cameras::GameplayCameraOp>(&fixture.name, req),
        "rigvm_graph_ops" => to_json::<rigvm::RigVmGraphOp>(&fixture.name, req),
        "pose_asset_ops" => to_json::<animation::PoseAssetOp>(&fixture.name, req),
        "mirror_table_ops" => to_json::<animation::MirrorTableOp>(&fixture.name, req),
        "rvt_ops" => to_json::<world::RvtOp>(&fixture.name, req),
        "media_ops" => to_json::<content::MediaOp>(&fixture.name, req),
        "metasound_ops" => to_json::<metasound::MetaSoundOp>(&fixture.name, req),
        "movie_render" => to_json::<movierender::MovieRenderOp>(&fixture.name, req),
        "nav_ops" => to_json::<navigation::NavigationOp>(&fixture.name, req),
        "state_tree_ops" => to_json::<navigation::StateTreeOp>(&fixture.name, req),
        "niagara_author" => to_json::<niagara::NiagaraAuthorOp>(&fixture.name, req),
        "anim_asset_ops" => to_json::<animation::AnimAssetOp>(&fixture.name, req),
        "anim_notify_ops" => to_json::<animation::AnimNotifyOp>(&fixture.name, req),
        "skeleton_ops" => to_json::<animation::SkeletonOp>(&fixture.name, req),
        "skeletal_mesh_ops" => to_json::<animation::SkeletalMeshOp>(&fixture.name, req),
        "physics_asset_ops" => to_json::<animation::PhysicsAssetOp>(&fixture.name, req),
        "material_graph" => to_json::<material_graph::MaterialGraphOp>(&fixture.name, req),
        "material_function" => to_json::<render::MaterialFunctionOp>(&fixture.name, req),
        "material_layers" => to_json::<render::MaterialLayerOp>(&fixture.name, req),
        "render_ops" => to_json::<render::RenderOp>(&fixture.name, req),
        "sequence_ops" => to_json::<sequences::SequenceOp>(&fixture.name, req),
        "landscape_ops" => to_json::<world::LandscapeOp>(&fixture.name, req),
        "sublevel_ops" => to_json::<world::SublevelOp>(&fixture.name, req),
        "world_partition_ops" => to_json::<world::WorldPartitionOp>(&fixture.name, req),
        "level_instance_ops" => to_json::<world::LevelInstanceOp>(&fixture.name, req),
        "foliage_ops" => to_json::<world::FoliageOp>(&fixture.name, req),
        "localization_ops" => {
            to_json::<mcp_unreal::headless::localization::LocalizationOp>(&fixture.name, req)
        }
        "source_control_ops" => to_json::<workflow::SourceControlOp>(&fixture.name, req),
        "validate_ops" => to_json::<workflow::ValidateOp>(&fixture.name, req),
        "gameplay_tag_ops" => to_json::<workflow::GameplayTagOp>(&fixture.name, req),
        "curve_ops" => to_json::<workflow::CurveOp>(&fixture.name, req),
        "reference_ops" => to_json::<workflow::ReferenceOp>(&fixture.name, req),
        "python_exec" => to_json::<python::PythonExecInput>(&fixture.name, req),
        "toolset_ops" => to_json::<toolsets::ToolsetOp>(&fixture.name, req),
        "collection_ops" => to_json::<workflow::CollectionOp>(&fixture.name, req),
        "editor_utility_ops" => to_json::<workflow::EditorUtilityOp>(&fixture.name, req),
        "asset_manager_ops" => to_json::<workflow::AssetManagerOp>(&fixture.name, req),
        "live_coding_ops" => to_json::<workflow::LiveCodingOp>(&fixture.name, req),
        "ddc_ops" => to_json::<workflow::DdcOp>(&fixture.name, req),
        "string_table_ops" => to_json::<content::StringTableOp>(&fixture.name, req),
        "curve_table_ops" => to_json::<content::CurveTableOp>(&fixture.name, req),
        // `status` probes the plugin with no body of its own.
        "status" => return None,
        other => panic!(
            "fixture '{}' names tool '{other}', which contract_tests does not know how to build a \
             body for — add it to wire_body",
            fixture.name
        ),
    };
    Some(body)
}

fn from_json<T: serde::de::DeserializeOwned>(name: &str, value: Value) -> T {
    serde_json::from_value(value)
        .unwrap_or_else(|e| panic!("fixture '{name}': request does not fit its tool's input: {e}"))
}

fn to_json<T: serde::de::DeserializeOwned + serde::Serialize>(name: &str, value: Value) -> Value {
    let typed: T = from_json(name, value);
    serde_json::to_value(typed).expect("serialize tool input")
}

/// A field renamed in Rust would make the built body disagree with what the
/// plugin was recorded accepting.
#[test]
fn fixture_requests_match_the_wire_body_the_tools_build() {
    for fixture in load_fixtures() {
        let Some(body) = wire_body(&fixture) else {
            continue;
        };
        assert_eq!(
            strip_top_level_nulls(&body),
            strip_top_level_nulls(&fixture.request),
            "fixture '{}': the body {} builds does not match the request the plugin was recorded \
             accepting",
            fixture.name,
            fixture.tool
        );
    }
}

/// Every fixture is a well-formed envelope: `ok` decides whether `data` or
/// `error {code, message}` is present, and error fixtures carry a non-2xx status.
#[test]
fn fixture_envelopes_are_well_formed() {
    for fixture in load_fixtures() {
        let envelope = fixture
            .response
            .as_object()
            .unwrap_or_else(|| panic!("fixture '{}': response is not an object", fixture.name));
        let ok = envelope
            .get("ok")
            .and_then(Value::as_bool)
            .unwrap_or_else(|| panic!("fixture '{}': envelope has no bool 'ok'", fixture.name));
        if ok {
            assert!(
                envelope.contains_key("data"),
                "fixture '{}': ok envelope without 'data'",
                fixture.name
            );
            assert!(
                (200..300).contains(&fixture.status),
                "fixture '{}': ok envelope with status {}",
                fixture.name,
                fixture.status
            );
        } else {
            let error = envelope
                .get("error")
                .and_then(Value::as_object)
                .unwrap_or_else(|| {
                    panic!(
                        "fixture '{}': error envelope without 'error' object",
                        fixture.name
                    )
                });
            for key in ["code", "message"] {
                let value = error.get(key).and_then(Value::as_str).unwrap_or_else(|| {
                    panic!("fixture '{}': error.{key} is not a string", fixture.name)
                });
                assert!(
                    !value.is_empty(),
                    "fixture '{}': error.{key} is empty",
                    fixture.name
                );
            }
            assert!(
                fixture.status >= 400,
                "fixture '{}': error envelope with status {}",
                fixture.name,
                fixture.status
            );
        }
        assert!(
            fixture.route.starts_with("/api/"),
            "fixture '{}': route '{}' is not an /api/ path",
            fixture.name,
            fixture.route
        );
    }
}

/// The client must unwrap each recorded envelope the way the tools expect:
/// success yields exactly `data`, failure an `Api` error with the plugin's own
/// code and message.
#[tokio::test]
async fn client_unwraps_every_recorded_envelope() {
    for fixture in load_fixtures() {
        let server = MockServer::start().await;
        Mock::given(method("POST"))
            .and(path(fixture.route.clone()))
            .and(body_json(fixture.request.clone()))
            .respond_with(
                ResponseTemplate::new(fixture.status).set_body_json(fixture.response.clone()),
            )
            .mount(&server)
            .await;

        let port = server.address().port();
        let client = EditorClient::new(port);
        let result = client.post(&fixture.route, fixture.request.clone()).await;

        let envelope = fixture.response.as_object().expect("object envelope");
        let ok = envelope["ok"].as_bool().expect("bool ok");
        match (ok, result) {
            (true, Ok(data)) => assert_eq!(
                data, envelope["data"],
                "fixture '{}': client returned something other than the envelope's data",
                fixture.name
            ),
            (false, Err(EditorError::Api { code, message })) => {
                let error = envelope["error"].as_object().expect("error object");
                assert_eq!(
                    message,
                    error["message"].as_str().unwrap(),
                    "fixture '{}': error message was not passed through",
                    fixture.name
                );
                assert_eq!(
                    code,
                    Some(fixture.status),
                    "fixture '{}': error lost its HTTP status",
                    fixture.name
                );
            }
            (true, Err(e)) => panic!("fixture '{}': ok envelope became error {e}", fixture.name),
            (false, Ok(v)) => {
                panic!(
                    "fixture '{}': error envelope became success {v}",
                    fixture.name
                )
            }
            (false, Err(e)) => panic!(
                "fixture '{}': error envelope became {e}, expected a plugin Api error",
                fixture.name
            ),
        }
    }
}

/// An unreachable plugin must produce remediation text naming the port, not a
/// raw reqwest error.
#[tokio::test]
async fn unreachable_plugin_reports_remediation() {
    // Port 1 is privileged and never served by the plugin.
    let client = EditorClient::new(1);
    let error = client
        .post("/api/status", Value::Null)
        .await
        .expect_err("connection to port 1 must fail");
    assert!(
        matches!(error, EditorError::Unreachable(_)),
        "expected Unreachable, got {error}"
    );
    let tool_error = error.into_tool_error(1);
    let message = tool_error.message.to_string();
    assert!(
        message.contains('1'),
        "remediation should mention the port: {message}"
    );
    assert!(
        message.to_lowercase().contains("editor"),
        "remediation should tell the user to start the editor: {message}"
    );
}

/// A non-JSON body from something that is not the plugin (a proxy, a different
/// service on the port) must not be reported as a successful call.
#[tokio::test]
async fn non_json_body_is_rejected() {
    let server = MockServer::start().await;
    Mock::given(method("POST"))
        .and(path("/api/status"))
        .respond_with(ResponseTemplate::new(200).set_body_string("<html>not the plugin</html>"))
        .mount(&server)
        .await;
    let client = EditorClient::new(server.address().port());
    let error = client
        .post("/api/status", serde_json::json!({}))
        .await
        .expect_err("html body must not be treated as a result");
    assert!(
        matches!(error, EditorError::BadResponse(_)),
        "expected BadResponse, got {error}"
    );
}
