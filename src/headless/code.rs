//! `code_ops` — C++ class and module scaffolding.
//!
//! Headless by design: the class/module templates are the engine's own
//! (`Engine/Content/Editor/Templates/*.template`), token-substituted exactly
//! the way `GameProjectUtils` does it, so generated code matches what the
//! editor's "New C++ Class" wizard would have written. After scaffolding, run
//! `generate_project_files` and `build_project`.

use std::path::{Path, PathBuf};

use rmcp::handler::server::wrapper::{Json, Parameters};
use rmcp::{ErrorData, tool, tool_router};
use serde_json::{Value, json};

use crate::config::ProjectInfo;
use crate::{UnrealMcp, error};

/// Which engine template pair a class is generated from.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub(crate) enum ClassTemplate {
    Actor,
    Character,
    Pawn,
    ActorComponent,
    Interface,
    UObject,
    Empty,
}

impl ClassTemplate {
    fn stem(self) -> &'static str {
        match self {
            Self::Actor => "ActorClass",
            Self::Character => "CharacterClass",
            Self::Pawn => "PawnClass",
            Self::ActorComponent => "ActorComponentClass",
            Self::Interface => "InterfaceClass",
            Self::UObject => "UObjectClass",
            Self::Empty => "EmptyClass",
        }
    }
}

/// A base class an agent can name without also supplying its header.
pub(crate) struct BaseClass {
    /// Prefixed engine name, e.g. "AActor".
    pub prefixed: &'static str,
    /// Header to include, relative to a module's include root.
    pub header: &'static str,
    pub template: ClassTemplate,
}

/// The engine classes worth scaffolding from. Anything absent is still usable
/// by passing `base_header` alongside a prefixed `base`.
pub(crate) const BASE_CLASSES: &[BaseClass] = &[
    // --- Actors ---
    BaseClass {
        prefixed: "AActor",
        header: "GameFramework/Actor.h",
        template: ClassTemplate::Actor,
    },
    BaseClass {
        prefixed: "APawn",
        header: "GameFramework/Pawn.h",
        template: ClassTemplate::Pawn,
    },
    BaseClass {
        prefixed: "ACharacter",
        header: "GameFramework/Character.h",
        template: ClassTemplate::Character,
    },
    BaseClass {
        prefixed: "ADefaultPawn",
        header: "GameFramework/DefaultPawn.h",
        template: ClassTemplate::Pawn,
    },
    BaseClass {
        prefixed: "ASpectatorPawn",
        header: "GameFramework/SpectatorPawn.h",
        template: ClassTemplate::Pawn,
    },
    BaseClass {
        prefixed: "AController",
        header: "GameFramework/Controller.h",
        template: ClassTemplate::Actor,
    },
    BaseClass {
        prefixed: "APlayerController",
        header: "GameFramework/PlayerController.h",
        template: ClassTemplate::Actor,
    },
    BaseClass {
        prefixed: "AAIController",
        header: "AIController.h",
        template: ClassTemplate::Actor,
    },
    BaseClass {
        prefixed: "AGameModeBase",
        header: "GameFramework/GameModeBase.h",
        template: ClassTemplate::Actor,
    },
    BaseClass {
        prefixed: "AGameMode",
        header: "GameFramework/GameMode.h",
        template: ClassTemplate::Actor,
    },
    BaseClass {
        prefixed: "AGameStateBase",
        header: "GameFramework/GameStateBase.h",
        template: ClassTemplate::Actor,
    },
    BaseClass {
        prefixed: "AGameState",
        header: "GameFramework/GameState.h",
        template: ClassTemplate::Actor,
    },
    BaseClass {
        prefixed: "APlayerState",
        header: "GameFramework/PlayerState.h",
        template: ClassTemplate::Actor,
    },
    BaseClass {
        prefixed: "AHUD",
        header: "GameFramework/HUD.h",
        template: ClassTemplate::Actor,
    },
    BaseClass {
        prefixed: "AWorldSettings",
        header: "GameFramework/WorldSettings.h",
        template: ClassTemplate::Actor,
    },
    BaseClass {
        prefixed: "AStaticMeshActor",
        header: "Engine/StaticMeshActor.h",
        template: ClassTemplate::Actor,
    },
    BaseClass {
        prefixed: "ACameraActor",
        header: "Camera/CameraActor.h",
        template: ClassTemplate::Actor,
    },
    BaseClass {
        prefixed: "AVolume",
        header: "GameFramework/Volume.h",
        template: ClassTemplate::Actor,
    },
    BaseClass {
        prefixed: "ATriggerBox",
        header: "Engine/TriggerBox.h",
        template: ClassTemplate::Actor,
    },
    // --- Components ---
    BaseClass {
        prefixed: "UActorComponent",
        header: "Components/ActorComponent.h",
        template: ClassTemplate::ActorComponent,
    },
    BaseClass {
        prefixed: "USceneComponent",
        header: "Components/SceneComponent.h",
        template: ClassTemplate::ActorComponent,
    },
    BaseClass {
        prefixed: "UPrimitiveComponent",
        header: "Components/PrimitiveComponent.h",
        template: ClassTemplate::ActorComponent,
    },
    BaseClass {
        prefixed: "UStaticMeshComponent",
        header: "Components/StaticMeshComponent.h",
        template: ClassTemplate::ActorComponent,
    },
    BaseClass {
        prefixed: "USkeletalMeshComponent",
        header: "Components/SkeletalMeshComponent.h",
        template: ClassTemplate::ActorComponent,
    },
    BaseClass {
        prefixed: "UCharacterMovementComponent",
        header: "GameFramework/CharacterMovementComponent.h",
        template: ClassTemplate::ActorComponent,
    },
    BaseClass {
        prefixed: "UCameraComponent",
        header: "Camera/CameraComponent.h",
        template: ClassTemplate::ActorComponent,
    },
    BaseClass {
        prefixed: "USpringArmComponent",
        header: "GameFramework/SpringArmComponent.h",
        template: ClassTemplate::ActorComponent,
    },
    // --- Plain UObjects ---
    BaseClass {
        prefixed: "UObject",
        header: "UObject/NoExportTypes.h",
        template: ClassTemplate::UObject,
    },
    BaseClass {
        prefixed: "UDataAsset",
        header: "Engine/DataAsset.h",
        template: ClassTemplate::UObject,
    },
    BaseClass {
        prefixed: "UPrimaryDataAsset",
        header: "Engine/DataAsset.h",
        template: ClassTemplate::UObject,
    },
    BaseClass {
        prefixed: "UDeveloperSettings",
        header: "Engine/DeveloperSettings.h",
        template: ClassTemplate::UObject,
    },
    BaseClass {
        prefixed: "UBlueprintFunctionLibrary",
        header: "Kismet/BlueprintFunctionLibrary.h",
        template: ClassTemplate::UObject,
    },
    BaseClass {
        prefixed: "USaveGame",
        header: "GameFramework/SaveGame.h",
        template: ClassTemplate::UObject,
    },
    BaseClass {
        prefixed: "UGameInstance",
        header: "Engine/GameInstance.h",
        template: ClassTemplate::UObject,
    },
    BaseClass {
        prefixed: "UGameInstanceSubsystem",
        header: "Subsystems/GameInstanceSubsystem.h",
        template: ClassTemplate::UObject,
    },
    BaseClass {
        prefixed: "UWorldSubsystem",
        header: "Subsystems/WorldSubsystem.h",
        template: ClassTemplate::UObject,
    },
    BaseClass {
        prefixed: "ULocalPlayerSubsystem",
        header: "Subsystems/LocalPlayerSubsystem.h",
        template: ClassTemplate::UObject,
    },
    BaseClass {
        prefixed: "UEngineSubsystem",
        header: "Subsystems/EngineSubsystem.h",
        template: ClassTemplate::UObject,
    },
    BaseClass {
        prefixed: "UEditorSubsystem",
        header: "EditorSubsystem.h",
        template: ClassTemplate::UObject,
    },
    BaseClass {
        prefixed: "UUserWidget",
        header: "Blueprint/UserWidget.h",
        template: ClassTemplate::UObject,
    },
    BaseClass {
        prefixed: "UAnimInstance",
        header: "Animation/AnimInstance.h",
        template: ClassTemplate::UObject,
    },
    BaseClass {
        prefixed: "UAnimNotify",
        header: "Animation/AnimNotifies/AnimNotify.h",
        template: ClassTemplate::UObject,
    },
    BaseClass {
        prefixed: "UAnimNotifyState",
        header: "Animation/AnimNotifies/AnimNotifyState.h",
        template: ClassTemplate::UObject,
    },
    BaseClass {
        prefixed: "UBTTaskNode",
        header: "BehaviorTree/BTTaskNode.h",
        template: ClassTemplate::UObject,
    },
    BaseClass {
        prefixed: "UBTService",
        header: "BehaviorTree/BTService.h",
        template: ClassTemplate::UObject,
    },
    BaseClass {
        prefixed: "UBTDecorator",
        header: "BehaviorTree/BTDecorator.h",
        template: ClassTemplate::UObject,
    },
    BaseClass {
        prefixed: "UGameplayAbility",
        header: "Abilities/GameplayAbility.h",
        template: ClassTemplate::UObject,
    },
    BaseClass {
        prefixed: "UAttributeSet",
        header: "AttributeSet.h",
        template: ClassTemplate::UObject,
    },
    BaseClass {
        prefixed: "UGameplayEffect",
        header: "GameplayEffect.h",
        template: ClassTemplate::UObject,
    },
    BaseClass {
        prefixed: "UAbilitySystemComponent",
        header: "AbilitySystemComponent.h",
        template: ClassTemplate::ActorComponent,
    },
    // --- Interface ---
    BaseClass {
        prefixed: "UInterface",
        header: "UObject/Interface.h",
        template: ClassTemplate::Interface,
    },
];

/// Look a base class up by prefixed ("AActor") or unprefixed ("Actor") name.
pub(crate) fn find_base(spec: &str) -> Option<&'static BaseClass> {
    BASE_CLASSES
        .iter()
        .find(|b| b.prefixed.eq_ignore_ascii_case(spec))
        .or_else(|| {
            BASE_CLASSES
                .iter()
                .find(|b| b.prefixed[1..].eq_ignore_ascii_case(spec))
        })
}

#[derive(serde::Deserialize, schemars::JsonSchema)]
#[serde(tag = "operation", rename_all = "snake_case")]
#[schemars(transform = crate::schema::object_with_oneof)]
pub enum CodeOp {
    /// Every C++ module in the project: name, type, loading phase and source
    /// folder, from the .uproject and the Build.cs files on disk.
    ListModules {},
    /// The shortlist of common base classes. Not exhaustive: `create_class`
    /// also resolves any reflected class in the installed engine source (and
    /// anything `lookup_class` can find) straight from the class index.
    ListBaseClasses { name_contains: Option<String> },
    /// Scaffold a new UCLASS (or plain C++ class) from the engine templates.
    CreateClass {
        /// Unprefixed name, e.g. "MyActor". The A/U/I prefix comes from the base.
        name: String,
        /// Base class, prefixed or not: "Actor", "AActor", "UObject",
        /// "ActorComponent", "UInterface" — or any reflected engine class, whose
        /// header and ancestry come from the engine class index. Omit for a
        /// plain non-UObject class.
        base: Option<String>,
        /// Header for a base the class index cannot see (a class in this
        /// project's own C++, say): "MyGame/Thing.h". `base` must then carry its
        /// prefix, since that is where the new class's prefix comes from.
        base_header: Option<String>,
        /// Module to add the class to. Defaults to the project's first module.
        module: Option<String>,
        /// Subfolder under the module's source root, e.g. "Weapons".
        subdirectory: Option<String>,
        /// Emit the `<MODULE>_API` export macro (default true).
        public_api: Option<bool>,
        /// UCLASS specifiers, e.g. "Blueprintable, BlueprintType".
        class_specifiers: Option<String>,
        /// Overwrite files that already exist (default false).
        overwrite: Option<bool>,
    },
    /// Scaffold a new C++ module: Build.cs, module implementation, folders,
    /// the .uproject entry and the target ExtraModuleNames registration.
    CreateModule {
        name: String,
        /// "Runtime" (default), "Editor", "UncookedOnly", "DeveloperTool",
        /// "Program", "ClientOnly", "ServerOnly".
        module_type: Option<String>,
        /// "Default" (default), "PreDefault", "PostEngineInit", "PostConfigInit".
        loading_phase: Option<String>,
        /// Extra PublicDependencyModuleNames (Core, CoreUObject, Engine are always added).
        public_dependencies: Option<Vec<String>>,
        private_dependencies: Option<Vec<String>>,
        /// Create Public/ and Private/ subfolders (default true).
        split_public_private: Option<bool>,
        /// Register only in the editor target, not the game target
        /// (use with module_type "Editor").
        editor_only: Option<bool>,
    },
}

#[tool_router(router = headless_code_router, vis = "pub(crate)")]
impl UnrealMcp {
    #[tool(
        description = "C++ scaffolding without the editor: list the project's modules, list the base classes available, create a new class from the engine's own class templates (Actor, Pawn, Character, ActorComponent, UObject, interface, plain class), or create a whole new module wired into the .uproject and the build targets. Files are written to disk only — follow with generate_project_files and build_project to compile them."
    )]
    async fn code_ops(&self, Parameters(op): Parameters<CodeOp>) -> Result<Json<Value>, ErrorData> {
        let project = self.cfg.project.as_ref().ok_or_else(error::no_project)?;
        match op {
            CodeOp::ListModules {} => Ok(Json(list_modules(project)?)),
            CodeOp::ListBaseClasses { name_contains } => {
                let needle = name_contains.unwrap_or_default().to_lowercase();
                let classes: Vec<Value> = BASE_CLASSES
                    .iter()
                    .filter(|b| needle.is_empty() || b.prefixed.to_lowercase().contains(&needle))
                    .map(|b| {
                        json!({
                            "base": b.prefixed,
                            "header": b.header,
                            "template": b.template.stem(),
                        })
                    })
                    .collect();
                Ok(Json(json!({ "count": classes.len(), "classes": classes })))
            }
            CodeOp::CreateClass {
                name,
                base,
                base_header,
                module,
                subdirectory,
                public_api,
                class_specifiers,
                overwrite,
            } => {
                let templates = self.template_dir()?;
                // The curated table answers the common bases outright; only a
                // miss pays for the engine-wide class index (first build of it
                // scans the whole engine source, then it is cached on disk).
                let resolved = match resolve_base(base.as_deref(), base_header.as_deref(), None) {
                    Ok(resolved) => resolved,
                    Err(BaseError::Invalid(message)) => return Err(error::invalid(message)),
                    Err(BaseError::TryIndex) => {
                        let index = self.docs.get().await;
                        resolve_base(base.as_deref(), base_header.as_deref(), Some(&index))
                            .map_err(|e| match e {
                                BaseError::Invalid(message) => error::invalid(message),
                                BaseError::TryIndex => {
                                    error::internal("base class resolution did not terminate")
                                }
                            })?
                    }
                };
                Ok(Json(create_class(
                    project,
                    &templates,
                    resolved,
                    CreateClassArgs {
                        name,
                        module,
                        subdirectory,
                        public_api: public_api.unwrap_or(true),
                        class_specifiers,
                        overwrite: overwrite.unwrap_or(false),
                    },
                )?))
            }
            CodeOp::CreateModule {
                name,
                module_type,
                loading_phase,
                public_dependencies,
                private_dependencies,
                split_public_private,
                editor_only,
            } => {
                let templates = self.template_dir()?;
                Ok(Json(create_module(
                    project,
                    &templates,
                    CreateModuleArgs {
                        name,
                        module_type: module_type.unwrap_or_else(|| "Runtime".into()),
                        loading_phase: loading_phase.unwrap_or_else(|| "Default".into()),
                        public_dependencies: public_dependencies.unwrap_or_default(),
                        private_dependencies: private_dependencies.unwrap_or_default(),
                        split_public_private: split_public_private.unwrap_or(true),
                        editor_only: editor_only.unwrap_or(false),
                    },
                )?))
            }
        }
    }

    fn template_dir(&self) -> Result<PathBuf, ErrorData> {
        let dir = self
            .cfg
            .engine_root
            .join("Engine")
            .join("Content")
            .join("Editor")
            .join("Templates");
        if !dir.is_dir() {
            return Err(error::missing_tool(&dir, "the engine class templates"));
        }
        Ok(dir)
    }
}

// ---------------------------------------------------------------- modules ---

/// A module's source directory: `Source/<Name>` next to the .uproject.
fn module_dir(project: &ProjectInfo, name: &str) -> PathBuf {
    project.root.join("Source").join(name)
}

fn read_uproject(project: &ProjectInfo) -> Result<Value, ErrorData> {
    let text = std::fs::read_to_string(&project.uproject)
        .map_err(|e| error::internal(format!("cannot read {}: {e}", project.uproject.display())))?;
    serde_json::from_str(&text).map_err(|e| {
        error::internal(format!(
            "{} is not valid JSON: {e}",
            project.uproject.display()
        ))
    })
}

fn list_modules(project: &ProjectInfo) -> Result<Value, ErrorData> {
    let uproject = read_uproject(project)?;
    let declared = uproject
        .get("Modules")
        .and_then(|m| m.as_array())
        .cloned()
        .unwrap_or_default();

    let mut modules = Vec::new();
    for entry in &declared {
        let name = entry
            .get("Name")
            .and_then(|n| n.as_str())
            .unwrap_or_default();
        let dir = module_dir(project, name);
        modules.push(json!({
            "name": name,
            "type": entry.get("Type").and_then(|t| t.as_str()).unwrap_or("Runtime"),
            "loading_phase": entry.get("LoadingPhase").and_then(|t| t.as_str()).unwrap_or("Default"),
            "source_dir": dir.display().to_string(),
            "exists": dir.is_dir(),
            "has_public_private": dir.join("Public").is_dir() && dir.join("Private").is_dir(),
        }));
    }

    // Build.cs files with no .uproject entry are a real and confusing failure
    // mode (the module silently never builds), so report them separately.
    let mut orphans = Vec::new();
    if let Ok(entries) = std::fs::read_dir(project.root.join("Source")) {
        for entry in entries.filter_map(|e| e.ok()) {
            let path = entry.path();
            if !path.is_dir() {
                continue;
            }
            let name = entry.file_name().to_string_lossy().into_owned();
            let has_build_cs = path.join(format!("{name}.Build.cs")).is_file();
            let declared_here = declared
                .iter()
                .any(|m| m.get("Name").and_then(|n| n.as_str()) == Some(name.as_str()));
            if has_build_cs && !declared_here {
                orphans.push(json!({ "name": name, "source_dir": path.display().to_string() }));
            }
        }
    }

    Ok(json!({
        "project": project.name,
        "count": modules.len(),
        "modules": modules,
        "unregistered_modules": orphans,
    }))
}

// ---------------------------------------------------------------- classes ---

struct CreateClassArgs {
    name: String,
    module: Option<String>,
    subdirectory: Option<String>,
    public_api: bool,
    class_specifiers: Option<String>,
    overwrite: bool,
}

/// Resolved base: prefix character, prefixed name, header, template.
#[derive(Debug)]
struct ResolvedBase {
    prefix: char,
    prefixed: String,
    header: Option<String>,
    template: ClassTemplate,
}

/// Why a base could not be resolved from the curated table alone.
#[derive(Debug)]
pub(crate) enum BaseError {
    /// The base is unknown here but the engine class index may know it.
    TryIndex,
    /// Nothing can rescue this one; the message names the fix.
    Invalid(String),
}

/// How an engine header is spelled in an `#include`: everything after the
/// module's `Public/`, `Classes/` or `Internal/` root, which is what UBT puts
/// on the include path.
pub(crate) fn include_path_for(header: &Path) -> String {
    let parts: Vec<String> = header
        .components()
        .map(|c| c.as_os_str().to_string_lossy().into_owned())
        .collect();
    let root = parts
        .iter()
        .rposition(|p| matches!(p.as_str(), "Public" | "Classes" | "Internal" | "Private"));
    match root {
        Some(index) => parts[index + 1..].join("/"),
        None => parts.last().cloned().unwrap_or_default(),
    }
}

/// Which template a base wants, from its ancestry in the engine class index.
/// Unknown ancestry falls back to the plain UObject template.
pub(crate) fn template_from_ancestry(
    index: &crate::docs::class_index::ClassIndex,
    base: &str,
) -> ClassTemplate {
    let mut name = base.to_string();
    for _ in 0..32 {
        if let Some(known) = find_base(&name) {
            return known.template;
        }
        match index.get(&name).and_then(|e| e.parent.clone()) {
            Some(parent) => name = parent,
            None => break,
        }
    }
    ClassTemplate::UObject
}

fn resolve_base(
    base: Option<&str>,
    base_header: Option<&str>,
    index: Option<&crate::docs::class_index::ClassIndex>,
) -> Result<ResolvedBase, BaseError> {
    let Some(spec) = base.filter(|s| !s.is_empty()) else {
        // No base at all: a plain, non-reflected C++ class.
        return Ok(ResolvedBase {
            prefix: 'F',
            prefixed: String::new(),
            header: None,
            template: ClassTemplate::Empty,
        });
    };
    if let Some(known) = find_base(spec) {
        return Ok(ResolvedBase {
            prefix: known.prefixed.chars().next().unwrap_or('U'),
            prefixed: known.prefixed.to_string(),
            header: Some(base_header.unwrap_or(known.header).to_string()),
            template: known.template,
        });
    }

    // An explicit header settles it: the prefix then has to come off the name.
    let prefix = spec.chars().next().unwrap_or('\0');
    if let Some(header) = base_header.filter(|h| !h.is_empty()) {
        if !matches!(prefix, 'A' | 'U' | 'I' | 'F' | 'S') {
            return Err(BaseError::Invalid(format!(
                "'{spec}' has no type prefix — with base_header the base must be spelled in full \
                 (AMyActor, UMyObject, IMyInterface)"
            )));
        }
        return Ok(ResolvedBase {
            prefix,
            prefixed: spec.to_string(),
            header: Some(header.to_string()),
            template: match prefix {
                'A' => ClassTemplate::Actor,
                'I' => ClassTemplate::Interface,
                'U' => ClassTemplate::UObject,
                _ => ClassTemplate::Empty,
            },
        });
    }

    // Otherwise the engine class index is the source of truth: it covers every
    // reflected class in the installed engine, not just the curated shortlist.
    let Some(index) = index else {
        return Err(BaseError::TryIndex);
    };
    let found = index.get(spec).map(|e| (spec.to_string(), e)).or_else(|| {
        ['A', 'U', 'I'].iter().find_map(|p| {
            let candidate = format!("{p}{spec}");
            index.get(&candidate).map(|e| (candidate, e))
        })
    });
    let Some((name, entry)) = found else {
        let suggestions = index.suggest(spec, 5);
        return Err(BaseError::Invalid(format!(
            "no class '{spec}' in the engine source index{} — pass base_header with the header \
             that declares it, or use code_ops list_base_classes",
            if suggestions.is_empty() {
                String::new()
            } else {
                format!(" (did you mean {}?)", suggestions.join(", "))
            }
        )));
    };
    let prefix = name.chars().next().unwrap_or('U');
    Ok(ResolvedBase {
        prefix,
        template: template_from_ancestry(index, &name),
        prefixed: name,
        header: Some(include_path_for(&entry.header)),
    })
}

/// `// <notice>` from the project's configured copyright, or an empty line.
fn copyright_line(project: &ProjectInfo) -> String {
    let ini = project.root.join("Config").join("DefaultGame.ini");
    let Ok(text) = std::fs::read_to_string(ini) else {
        return String::new();
    };
    text.lines()
        .find_map(|l| l.trim().strip_prefix("CopyrightNotice="))
        .map(|n| format!("// {}", n.trim()))
        .unwrap_or_default()
}

fn read_template(dir: &Path, file: &str) -> Result<String, ErrorData> {
    let path = dir.join(file);
    std::fs::read_to_string(&path)
        .map_err(|e| error::internal(format!("cannot read template {}: {e}", path.display())))
}

/// Substitute `%TOKEN%` pairs. Unknown tokens are left alone so a template
/// change surfaces visibly instead of silently dropping content.
pub(crate) fn fill(template: &str, tokens: &[(&str, &str)]) -> String {
    let mut out = template.to_string();
    for (key, value) in tokens {
        out = out.replace(&format!("%{key}%"), value);
    }
    out
}

/// Collapse the blank lines that empty tokens leave behind, and normalise the
/// trailing newline, so generated files look hand-written.
pub(crate) fn tidy(text: &str) -> String {
    let lines: Vec<&str> = text.lines().map(str::trim_end).collect();
    let mut out = String::with_capacity(text.len());
    let mut blank_run = 0;
    for (index, line) in lines.iter().enumerate() {
        if line.is_empty() {
            blank_run += 1;
            if blank_run > 1 {
                continue;
            }
            // An empty token between two includes leaves a gap the wizard's
            // output would not have.
            let between_includes = out
                .lines()
                .last()
                .is_some_and(|l| l.starts_with("#include"))
                && lines[index + 1..]
                    .first()
                    .is_some_and(|l| l.starts_with("#include"));
            if between_includes {
                continue;
            }
        } else {
            blank_run = 0;
        }
        out.push_str(line);
        out.push('\n');
    }
    while out.starts_with('\n') {
        out.remove(0);
    }
    out
}

fn create_class(
    project: &ProjectInfo,
    templates: &Path,
    base: ResolvedBase,
    args: CreateClassArgs,
) -> Result<Value, ErrorData> {
    if args.name.is_empty() || !args.name.chars().all(|c| c.is_alphanumeric() || c == '_') {
        return Err(error::invalid(format!(
            "class name '{}' must be alphanumeric — pass the unprefixed name, e.g. \"MyActor\"",
            args.name
        )));
    }
    let uproject = read_uproject(project)?;
    let module = match args.module {
        Some(m) => m,
        None => uproject
            .get("Modules")
            .and_then(|m| m.as_array())
            .and_then(|m| m.first())
            .and_then(|m| m.get("Name"))
            .and_then(|n| n.as_str())
            .map(str::to_string)
            .ok_or_else(|| {
                error::invalid(
                    "the .uproject declares no modules — run code_ops create_module first, \
                     or pass `module` explicitly",
                )
            })?,
    };
    let root = module_dir(project, &module);
    if !root.is_dir() {
        return Err(error::invalid(format!(
            "module '{module}' has no source folder at {} — see code_ops list_modules",
            root.display()
        )));
    }

    // Public/Private split when the module already uses one, flat otherwise.
    let split = root.join("Public").is_dir() && root.join("Private").is_dir();
    let sub = args.subdirectory.unwrap_or_default();
    let header_dir = if split {
        root.join("Public")
    } else {
        root.clone()
    }
    .join(&sub);
    let source_dir = if split {
        root.join("Private")
    } else {
        root.clone()
    }
    .join(&sub);

    let unprefixed = args.name.clone();
    // A plain C++ class carries no reflection prefix, matching what the
    // editor's own "None" base class produces.
    let prefixed = if base.template == ClassTemplate::Empty {
        unprefixed.clone()
    } else {
        format!("{}{unprefixed}", base.prefix)
    };
    let api_macro = if args.public_api {
        format!("{}_API ", module.to_uppercase())
    } else {
        String::new()
    };
    let base_include = base
        .header
        .as_ref()
        // InterfaceClass.h.template includes UObject/Interface.h itself, so
        // emitting the base include again would duplicate the line.
        .filter(|h| !(base.template == ClassTemplate::Interface && *h == "UObject/Interface.h"))
        .map(|h| format!("#include \"{h}\""))
        .unwrap_or_default();
    let specifiers = args.class_specifiers.unwrap_or_else(|| {
        if base.template == ClassTemplate::Interface {
            "MinimalAPI".into()
        } else {
            String::new()
        }
    });
    let header_name = format!("{unprefixed}.h");
    let copyright = copyright_line(project);

    let tokens: Vec<(&str, &str)> = vec![
        ("COPYRIGHT_LINE", copyright.as_str()),
        ("UNPREFIXED_CLASS_NAME", unprefixed.as_str()),
        ("PREFIXED_CLASS_NAME", prefixed.as_str()),
        ("PREFIXED_BASE_CLASS_NAME", base.prefixed.as_str()),
        ("BASE_CLASS_INCLUDE_DIRECTIVE", base_include.as_str()),
        ("CLASS_MODULE_API_MACRO", api_macro.as_str()),
        ("UCLASS_SPECIFIER_LIST", specifiers.as_str()),
        ("MY_HEADER_INCLUDE_DIRECTIVE", ""),
        ("PCH_INCLUDE_DIRECTIVE", ""),
        ("ADDITIONAL_INCLUDE_DIRECTIVES", ""),
        ("ADDITIONAL_MEMBER_DEFINITIONS", ""),
        ("CLASS_PROPERTIES", ""),
        ("CLASS_FUNCTION_DECLARATIONS", ""),
        ("CURSORFOCUSLOCATION", ""),
        ("EVENTUAL_CONSTRUCTOR_DECLARATION", ""),
        ("EVENTUAL_CONSTRUCTOR_DEFINITION", ""),
        ("PROPERTY_OVERRIDES", ""),
    ];

    let header_template =
        read_template(templates, &format!("{}.h.template", base.template.stem()))?;
    let source_template =
        read_template(templates, &format!("{}.cpp.template", base.template.stem()))?;

    let my_include = format!("#include \"{header_name}\"");
    let mut source_tokens = tokens.clone();
    for entry in &mut source_tokens {
        if entry.0 == "MY_HEADER_INCLUDE_DIRECTIVE" {
            entry.1 = my_include.as_str();
        }
    }

    let header_path = header_dir.join(&header_name);
    let source_path = source_dir.join(format!("{unprefixed}.cpp"));
    for path in [&header_path, &source_path] {
        if path.exists() && !args.overwrite {
            return Err(error::invalid(format!(
                "{} already exists — pass overwrite=true to replace it",
                path.display()
            )));
        }
    }
    for dir in [&header_dir, &source_dir] {
        std::fs::create_dir_all(dir)
            .map_err(|e| error::internal(format!("cannot create {}: {e}", dir.display())))?;
    }
    write_file(&header_path, &tidy(&fill(&header_template, &tokens)))?;
    write_file(&source_path, &tidy(&fill(&source_template, &source_tokens)))?;

    Ok(json!({
        "class": prefixed,
        "module": module,
        "base": if base.prefixed.is_empty() { Value::Null } else { json!(base.prefixed) },
        "template": base.template.stem(),
        "header": header_path.display().to_string(),
        "source": source_path.display().to_string(),
        "next": "run generate_project_files, then build_project, to compile the new class",
    }))
}

fn write_file(path: &Path, contents: &str) -> Result<(), ErrorData> {
    std::fs::write(path, contents)
        .map_err(|e| error::internal(format!("cannot write {}: {e}", path.display())))
}

// ---------------------------------------------------------- module creation ---

struct CreateModuleArgs {
    name: String,
    module_type: String,
    loading_phase: String,
    public_dependencies: Vec<String>,
    private_dependencies: Vec<String>,
    split_public_private: bool,
    editor_only: bool,
}

/// C# string-array literal body: `"Core", "CoreUObject"`.
fn cs_list(items: &[String]) -> String {
    items
        .iter()
        .map(|i| format!("\"{i}\""))
        .collect::<Vec<_>>()
        .join(", ")
}

fn create_module(
    project: &ProjectInfo,
    templates: &Path,
    args: CreateModuleArgs,
) -> Result<Value, ErrorData> {
    if args.name.is_empty() || !args.name.chars().all(|c| c.is_alphanumeric() || c == '_') {
        return Err(error::invalid(format!(
            "module name '{}' must be alphanumeric",
            args.name
        )));
    }
    let root = module_dir(project, &args.name);
    if root.exists() {
        return Err(error::invalid(format!(
            "{} already exists — pick another module name",
            root.display()
        )));
    }

    let mut public_deps = vec!["Core".to_string()];
    for dep in &args.public_dependencies {
        if !public_deps.contains(dep) {
            public_deps.push(dep.clone());
        }
    }
    let mut private_deps = vec!["CoreUObject".to_string(), "Engine".to_string()];
    if args.module_type.eq_ignore_ascii_case("Editor") {
        private_deps.push("UnrealEd".into());
        private_deps.push("Slate".into());
        private_deps.push("SlateCore".into());
    }
    for dep in &args.private_dependencies {
        if !private_deps.contains(dep) {
            private_deps.push(dep.clone());
        }
    }

    let copyright = copyright_line(project);
    let build_template = read_template(
        templates,
        if args.module_type.eq_ignore_ascii_case("Editor") {
            "EditorModule.Build.cs.template"
        } else {
            "GameModule.Build.cs.template"
        },
    )?;
    let public_list = cs_list(&public_deps);
    let private_list = cs_list(&private_deps);
    let build_cs = tidy(&fill(
        &build_template,
        &[
            ("COPYRIGHT_LINE", copyright.as_str()),
            ("MODULE_NAME", args.name.as_str()),
            ("PUBLIC_DEPENDENCY_MODULE_NAMES", public_list.as_str()),
            ("PRIVATE_DEPENDENCY_MODULE_NAMES", private_list.as_str()),
        ],
    ));

    let (public_dir, private_dir) = if args.split_public_private {
        (root.join("Public"), root.join("Private"))
    } else {
        (root.clone(), root.clone())
    };
    for dir in [&public_dir, &private_dir] {
        std::fs::create_dir_all(dir)
            .map_err(|e| error::internal(format!("cannot create {}: {e}", dir.display())))?;
    }

    let mut created = Vec::new();
    let build_path = root.join(format!("{}.Build.cs", args.name));
    write_file(&build_path, &build_cs)?;
    created.push(build_path.display().to_string());

    // A secondary game module implements the default module, not the primary
    // game module — only one target module may use IMPLEMENT_PRIMARY_GAME_MODULE.
    let module_cpp = format!(
        "{copyright}\n\n#include \"{name}.h\"\n#include \"Modules/ModuleManager.h\"\n\n\
         IMPLEMENT_MODULE(FDefaultModuleImpl, {name});\n",
        copyright = copyright,
        name = args.name
    );
    let module_h = format!(
        "{copyright}\n\n#pragma once\n\n#include \"CoreMinimal.h\"\n",
        copyright = copyright
    );
    let h_path = public_dir.join(format!("{}.h", args.name));
    let cpp_path = private_dir.join(format!("{}.cpp", args.name));
    write_file(&h_path, &tidy(&module_h))?;
    write_file(&cpp_path, &tidy(&module_cpp))?;
    created.push(h_path.display().to_string());
    created.push(cpp_path.display().to_string());

    // Register in the .uproject so the module is loaded at all.
    let uproject = read_uproject(project)?;
    let already_declared = uproject
        .get("Modules")
        .and_then(|m| m.as_array())
        .is_some_and(|list| {
            list.iter()
                .any(|m| m.get("Name").and_then(|n| n.as_str()) == Some(args.name.as_str()))
        });
    if already_declared {
        return Err(error::invalid(format!(
            "the .uproject already declares a module named '{}'",
            args.name
        )));
    }
    let text = std::fs::read_to_string(&project.uproject)
        .map_err(|e| error::internal(format!("cannot read {}: {e}", project.uproject.display())))?;
    let entry = format!(
        "{{\n\t\t\t\"Name\": \"{}\",\n\t\t\t\"Type\": \"{}\",\n\t\t\t\"LoadingPhase\": \"{}\"\n\t\t}}",
        args.name, args.module_type, args.loading_phase
    );
    let updated = insert_module_entry(&text, &entry).ok_or_else(|| {
        error::internal(format!(
            "could not find the \"Modules\" array in {} — add the module entry by hand",
            project.uproject.display()
        ))
    })?;
    write_file(&project.uproject, &updated)?;

    // Register with the build targets, or the module never compiles.
    let mut targets = Vec::new();
    let source = project.root.join("Source");
    for suffix in ["Editor.Target.cs", ".Target.cs"] {
        if args.editor_only && suffix == ".Target.cs" {
            continue;
        }
        let path = source.join(format!("{}{suffix}", project.name));
        if let Some(added) = add_to_target(&path, &args.name)? {
            targets.push(added);
        }
    }

    Ok(json!({
        "module": args.name,
        "type": args.module_type,
        "loading_phase": args.loading_phase,
        "source_dir": root.display().to_string(),
        "files": created,
        "targets_updated": targets,
        "uproject": project.uproject.display().to_string(),
        "next": "run generate_project_files, then build_project, to compile the new module",
    }))
}

/// Append `Name` to a target's `ExtraModuleNames`. Returns the target path
/// when it was changed, None when the file is absent or already lists it.
pub(crate) fn add_to_target(path: &Path, module: &str) -> Result<Option<String>, ErrorData> {
    let Ok(text) = std::fs::read_to_string(path) else {
        return Ok(None);
    };
    if text.contains(&format!("\"{module}\"")) {
        return Ok(None);
    }
    let insert = format!("\t\tExtraModuleNames.Add(\"{module}\");\n");
    // Anchor on the last existing ExtraModuleNames call so ordering matches
    // what a human would write; fall back to the end of the constructor.
    let updated = if let Some(pos) = text.rfind("ExtraModuleNames") {
        let line_end = text[pos..]
            .find('\n')
            .map(|i| pos + i + 1)
            .unwrap_or(text.len());
        format!("{}{insert}{}", &text[..line_end], &text[line_end..])
    } else if let Some(pos) = text.find('}') {
        format!("{}{insert}{}", &text[..pos], &text[pos..])
    } else {
        return Ok(None);
    };
    write_file(path, &updated)?;
    Ok(Some(path.display().to_string()))
}

/// Splice a module entry into a .uproject's `"Modules"` array without
/// reserialising the file. A project descriptor is hand-edited and source
/// controlled, so a rewrite that reorders every key is a bad diff. Returns
/// None when no `"Modules"` array can be located.
pub(crate) fn insert_module_entry(text: &str, entry: &str) -> Option<String> {
    let key = text.find("\"Modules\"")?;
    let open = key + text[key..].find('[')?;
    let mut depth = 0i32;
    let mut close = None;
    for (offset, ch) in text[open..].char_indices() {
        match ch {
            '[' | '{' => depth += 1,
            ']' | '}' => {
                depth -= 1;
                if depth == 0 {
                    close = Some(open + offset);
                    break;
                }
            }
            _ => {}
        }
    }
    let close = close?;
    let body = text[open + 1..close].trim_end();
    let inserted = if body.trim().is_empty() {
        format!("\n\t\t{entry}\n\t")
    } else {
        format!("{body},\n\t\t{entry}\n\t")
    };
    Some(format!("{}{inserted}{}", &text[..open + 1], &text[close..]))
}

#[cfg(test)]
mod tests {
    use super::*;

    use crate::docs::class_index::{ClassIndex, SymbolEntry};
    use std::collections::HashMap;

    fn err_message(e: BaseError) -> String {
        match e {
            BaseError::Invalid(m) => m,
            BaseError::TryIndex => "try-index".into(),
        }
    }

    /// A stand-in for the engine index: MyThing -> MyBase -> AActor.
    fn fake_index() -> ClassIndex {
        let mut symbols = HashMap::new();
        symbols.insert(
            "AMyThing".to_string(),
            SymbolEntry {
                kind: "class".into(),
                header: PathBuf::from("Game/Source/Game/Public/Things/MyThing.h"),
                module: "Game".into(),
                parent: Some("AMyBase".into()),
                line: 10,
            },
        );
        symbols.insert(
            "AMyBase".to_string(),
            SymbolEntry {
                kind: "class".into(),
                header: PathBuf::from("Game/Source/Game/Public/MyBase.h"),
                module: "Game".into(),
                parent: Some("AActor".into()),
                line: 10,
            },
        );
        ClassIndex {
            engine_version: "5.8.0".into(),
            symbols,
            headers_scanned: 2,
        }
    }

    #[test]
    fn known_bases_resolve_by_prefixed_and_unprefixed_name() {
        let a = resolve_base(Some("Actor"), None, None).unwrap();
        assert_eq!(a.prefixed, "AActor");
        assert_eq!(a.prefix, 'A');
        assert_eq!(a.template, ClassTemplate::Actor);

        let u = resolve_base(Some("UObject"), None, None).unwrap();
        assert_eq!(u.prefix, 'U');
        assert_eq!(u.template, ClassTemplate::UObject);

        let i = resolve_base(Some("UInterface"), None, None).unwrap();
        assert_eq!(i.template, ClassTemplate::Interface);
    }

    #[test]
    fn no_base_means_a_plain_class() {
        let b = resolve_base(None, None, None).unwrap();
        assert_eq!(b.template, ClassTemplate::Empty);
        assert!(b.prefixed.is_empty());
        assert!(b.header.is_none());
    }

    #[test]
    fn an_unknown_base_defers_to_the_class_index() {
        assert!(matches!(
            resolve_base(Some("AMyThing"), None, None),
            Err(BaseError::TryIndex)
        ));
    }

    #[test]
    fn explicit_header_needs_a_prefixed_base() {
        let err = err_message(resolve_base(Some("MyThing"), Some("X/Y.h"), None).unwrap_err());
        assert!(err.contains("type prefix"), "{err}");

        let ok = resolve_base(Some("AMyThing"), Some("MyGame/MyThing.h"), None).unwrap();
        assert_eq!(ok.prefix, 'A');
        assert_eq!(ok.header.as_deref(), Some("MyGame/MyThing.h"));
        assert_eq!(ok.template, ClassTemplate::Actor);
    }

    #[test]
    fn base_header_overrides_the_builtin_header() {
        let b = resolve_base(Some("AActor"), Some("Custom/Actor.h"), None).unwrap();
        assert_eq!(b.header.as_deref(), Some("Custom/Actor.h"));
    }

    #[test]
    fn the_index_supplies_header_prefix_and_template() {
        let index = fake_index();
        let b = resolve_base(Some("AMyThing"), None, Some(&index)).unwrap();
        assert_eq!(b.prefixed, "AMyThing");
        assert_eq!(b.prefix, 'A');
        assert_eq!(b.header.as_deref(), Some("Things/MyThing.h"));
        // Ancestry walks AMyThing -> AMyBase -> AActor, so it is an Actor.
        assert_eq!(b.template, ClassTemplate::Actor);

        // The unprefixed spelling finds it too.
        let b = resolve_base(Some("MyThing"), None, Some(&index)).unwrap();
        assert_eq!(b.prefixed, "AMyThing");
    }

    #[test]
    fn an_index_miss_names_near_matches() {
        let index = fake_index();
        let err = err_message(resolve_base(Some("MyThng"), None, Some(&index)).unwrap_err());
        assert!(err.contains("no class 'MyThng'"), "{err}");
    }

    #[test]
    fn include_paths_start_after_the_module_include_root() {
        assert_eq!(
            include_path_for(Path::new(
                "E/Source/Runtime/Engine/Classes/GameFramework/Actor.h"
            )),
            "GameFramework/Actor.h"
        );
        assert_eq!(
            include_path_for(Path::new("E/Source/Runtime/Engine/Public/EngineUtils.h")),
            "EngineUtils.h"
        );
        // No recognisable root: fall back to the bare file name.
        assert_eq!(include_path_for(Path::new("odd/place/Thing.h")), "Thing.h");
    }

    #[test]
    fn fill_substitutes_and_leaves_unknown_tokens() {
        let out = fill("%A% and %B%", &[("A", "one")]);
        assert_eq!(out, "one and %B%");
    }

    #[test]
    fn tidy_collapses_blank_runs_and_leading_blanks() {
        assert_eq!(tidy("\n\nint x;\n\n\n\nint y;\n"), "int x;\n\nint y;\n");
        assert_eq!(tidy("a   \n"), "a\n");
    }

    #[test]
    fn tidy_closes_the_gap_an_empty_token_leaves_between_includes() {
        assert_eq!(
            tidy("#include \"a.h\"\n\n#include \"b.h\"\n\nvoid f();\n"),
            "#include \"a.h\"\n#include \"b.h\"\n\nvoid f();\n"
        );
    }

    #[test]
    fn cs_list_quotes_each_dependency() {
        assert_eq!(
            cs_list(&["Core".into(), "Engine".into()]),
            "\"Core\", \"Engine\""
        );
        assert_eq!(cs_list(&[]), "");
    }

    #[test]
    fn add_to_target_appends_after_the_last_entry() {
        let dir = std::env::temp_dir().join(format!("mcp-code-{}", std::process::id()));
        std::fs::create_dir_all(&dir).unwrap();
        let path = dir.join("Demo.Target.cs");
        std::fs::write(
            &path,
            "public class DemoTarget : TargetRules\n{\n\tpublic DemoTarget()\n\t{\n\t\tExtraModuleNames.Add(\"Demo\");\n\t}\n}\n",
        )
        .unwrap();

        assert!(add_to_target(&path, "Extra").unwrap().is_some());
        let text = std::fs::read_to_string(&path).unwrap();
        assert!(text.contains("ExtraModuleNames.Add(\"Demo\");"));
        assert!(text.contains("ExtraModuleNames.Add(\"Extra\");"));
        assert!(
            text.find("\"Demo\"").unwrap() < text.find("\"Extra\"").unwrap(),
            "new module should follow the existing ones"
        );

        // Idempotent: a module already listed is left alone.
        assert!(add_to_target(&path, "Extra").unwrap().is_none());
        std::fs::remove_dir_all(&dir).ok();
    }

    #[test]
    fn module_entry_is_spliced_into_an_existing_array() {
        let text = "{\n\t\"Modules\": [\n\t\t{\n\t\t\t\"Name\": \"Game\"\n\t\t}\n\t],\n\t\"Plugins\": []\n}\n";
        let out = insert_module_entry(text, "{\"Name\": \"Tools\"}").unwrap();
        // Everything outside the Modules array survives byte for byte.
        assert!(out.contains("\"Plugins\": []"));
        assert!(out.find("Game").unwrap() < out.find("Tools").unwrap());
        let parsed: Value = serde_json::from_str(&out).expect("still valid JSON");
        assert_eq!(parsed["Modules"].as_array().unwrap().len(), 2);
    }

    #[test]
    fn module_entry_is_spliced_into_an_empty_array() {
        let text = "{\n\t\"Modules\": [],\n\t\"X\": 1\n}\n";
        let out = insert_module_entry(text, "{\"Name\": \"Tools\"}").unwrap();
        let parsed: Value = serde_json::from_str(&out).expect("still valid JSON");
        assert_eq!(parsed["Modules"].as_array().unwrap().len(), 1);
        assert_eq!(parsed["X"], 1);
    }

    #[test]
    fn missing_modules_array_is_reported_not_guessed() {
        assert!(insert_module_entry("{\"Plugins\": []}", "{}").is_none());
    }

    #[test]
    fn add_to_target_ignores_a_missing_file() {
        assert!(
            add_to_target(Path::new("does/not/exist.Target.cs"), "X")
                .unwrap()
                .is_none()
        );
    }
}
