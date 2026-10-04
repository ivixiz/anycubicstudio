//! Independent mesh-to-PWSZ slicer.
//!
//! The PWSZ container and vector layer encoder are based on the observable
//! Photon Workshop 4.1.8 file contract. STEP/IGES tessellation is delegated to
//! Gmsh; STL and the mesh subset of 3MF are decoded directly by this crate.

mod ffi;
mod geometry;
mod mesh;
pub mod pcb;
mod render;
mod support;
mod viewer_scene;
mod writer;

use std::path::{Path, PathBuf};

use serde::{Deserialize, Serialize};

use crate::{Error, Result};

use self::{
    geometry::slice_mesh, mesh::Mesh, support::generate_supports, viewer_scene::write_viewer_scene,
    writer::write_pwsz,
};

pub use ffi::{AnycubicSlicerConfig, AnycubicSupportPoint};

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct MachineConfig {
    pub name: String,
    pub resolution: [u32; 2],
    pub build_volume_mm: [f64; 3],
    pub pixel_size_um: [f64; 2],
    pub max_segments_per_layer: usize,
}

impl MachineConfig {
    /// Profile recovered from the supplied Photon Mono M7 Pro PWSZ sample.
    pub fn photon_mono_m7_pro() -> Self {
        Self {
            name: "Anycubic Photon Mono M7 Pro".into(),
            resolution: [13_312, 5_120],
            build_volume_mm: [223.64199829101563, 126.4800033569336, 230.0],
            pixel_size_um: [16.799999237060547, 24.799999237060547],
            max_segments_per_layer: 100_000,
        }
    }
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct PrintConfig {
    pub layer_height_mm: f64,
    pub bottom_layers: u32,
    pub bottom_exposure_s: f64,
    pub exposure_s: f64,
    pub lift_height_mm: f64,
    pub lift_speed_mm_s: f64,
    pub off_time_s: f64,
    pub resin_density_g_ml: f64,
    pub resin_price: f64,
    pub resin_volume_ml: f64,
    pub currency: String,
}

impl Default for PrintConfig {
    fn default() -> Self {
        Self {
            layer_height_mm: 0.05,
            bottom_layers: 5,
            bottom_exposure_s: 28.0,
            exposure_s: 1.8,
            lift_height_mm: 8.0,
            lift_speed_mm_s: 6.0,
            off_time_s: 0.5,
            resin_density_g_ml: 1.2,
            resin_price: 220.0,
            resin_volume_ml: 1000.0,
            currency: "$".into(),
        }
    }
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct Transform {
    pub translation_mm: [f64; 3],
    pub rotation_deg: [f64; 3],
    pub scale: [f64; 3],
    pub auto_center_xy: bool,
    pub place_on_bed: bool,
}

impl Default for Transform {
    fn default() -> Self {
        Self {
            translation_mm: [0.0; 3],
            rotation_deg: [0.0; 3],
            scale: [1.0; 3],
            auto_center_xy: true,
            place_on_bed: true,
        }
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum SupportShape {
    Cylinder,
    Tapered,
    Square,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct SupportPoint {
    pub contact_mm: [f64; 3],
    /// `None` lets the generator find the nearest model surface below the
    /// contact, falling back to the build plate.
    pub base_z_mm: Option<f64>,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct SupportConfig {
    pub enabled: bool,
    pub auto_generate: bool,
    pub shape: SupportShape,
    pub overhang_angle_deg: f64,
    pub spacing_mm: f64,
    pub tip_diameter_mm: f64,
    pub trunk_diameter_mm: f64,
    pub base_diameter_mm: f64,
    pub tip_height_mm: f64,
    pub base_height_mm: f64,
    pub penetration_mm: f64,
    pub min_height_mm: f64,
    pub max_height_mm: f64,
    pub radial_segments: u32,
    #[serde(default)]
    pub manual_points: Vec<SupportPoint>,
}

impl SupportConfig {
    pub fn light() -> Self {
        Self {
            enabled: false,
            auto_generate: true,
            shape: SupportShape::Tapered,
            overhang_angle_deg: 45.0,
            spacing_mm: 5.0,
            tip_diameter_mm: 0.35,
            trunk_diameter_mm: 0.8,
            base_diameter_mm: 3.5,
            tip_height_mm: 1.5,
            base_height_mm: 0.8,
            penetration_mm: 0.15,
            min_height_mm: 2.0,
            max_height_mm: 180.0,
            radial_segments: 12,
            manual_points: Vec::new(),
        }
    }

    pub fn medium() -> Self {
        Self {
            tip_diameter_mm: 0.5,
            trunk_diameter_mm: 1.2,
            base_diameter_mm: 4.5,
            spacing_mm: 4.0,
            ..Self::light()
        }
    }

    pub fn heavy() -> Self {
        Self {
            tip_diameter_mm: 0.8,
            trunk_diameter_mm: 1.8,
            base_diameter_mm: 6.0,
            spacing_mm: 3.0,
            tip_height_mm: 2.0,
            ..Self::light()
        }
    }

    pub fn add_manual_point(&mut self, contact_mm: [f64; 3], base_z_mm: Option<f64>) {
        self.manual_points.push(SupportPoint {
            contact_mm,
            base_z_mm,
        });
    }
}

impl Default for SupportConfig {
    fn default() -> Self {
        Self::medium()
    }
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct SlicerConfig {
    pub machine: MachineConfig,
    pub print: PrintConfig,
    pub transform: Transform,
    pub supports: SupportConfig,
    /// Reserved for a future robust polygon offset implementation.
    pub xy_offset_mm: f64,
    /// Reserved for a future hollowing implementation.
    pub hollow: bool,
    /// Executable used only for STEP/STP/IGES tessellation.
    pub gmsh_path: PathBuf,
}

/// One independently transformed and supported model in a multi-model plate.
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct SceneModelConfig {
    pub input: PathBuf,
    pub transform: Transform,
    pub supports: SupportConfig,
}

/// Serializable plate description used by the GUI and host applications.
/// Machine/print settings are shared while every model retains its own
/// transform and support profile.
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct SceneProject {
    pub config: SlicerConfig,
    pub models: Vec<SceneModelConfig>,
}

impl Default for SlicerConfig {
    fn default() -> Self {
        Self {
            machine: MachineConfig::photon_mono_m7_pro(),
            print: PrintConfig::default(),
            transform: Transform::default(),
            supports: SupportConfig::default(),
            xy_offset_mm: 0.0,
            hollow: false,
            gmsh_path: PathBuf::from("gmsh"),
        }
    }
}

#[derive(Debug, Clone)]
pub struct SliceReport {
    pub triangle_count: usize,
    pub model_triangle_count: usize,
    pub support_triangle_count: usize,
    pub support_count: usize,
    pub layer_count: u32,
    pub bounds_mm: [f64; 6],
    pub estimated_volume_ml: f64,
    pub output_bytes: u64,
}

#[derive(Debug, Clone)]
pub struct ViewerSceneReport {
    pub model_triangle_count: usize,
    pub support_triangle_count: usize,
    pub support_count: usize,
    pub layer_count: u32,
    pub output_bytes: u64,
}

struct PreparedScene {
    model: Mesh,
    supports: Mesh,
    combined: Mesh,
    layers: Vec<geometry::LayerSlice>,
    support_count: usize,
    bounds: [f64; 6],
    volume_mm3: f64,
}

/// Slices one STL, 3MF, STEP/STP or IGES model into a PWSZ file.
pub fn slice_file(
    input: impl AsRef<Path>,
    output: impl AsRef<Path>,
    config: &SlicerConfig,
) -> Result<SliceReport> {
    let input = input.as_ref();
    let output = output.as_ref();
    let scene = prepare_scene(input, config)?;
    write_pwsz(
        output,
        input,
        config,
        &scene.model,
        &scene.supports,
        &scene.layers,
        scene.volume_mm3,
    )?;
    Ok(SliceReport {
        triangle_count: scene.combined.triangles.len(),
        model_triangle_count: scene.model.triangles.len(),
        support_triangle_count: scene.supports.triangles.len(),
        support_count: scene.support_count,
        layer_count: scene.layers.len() as u32,
        bounds_mm: scene.bounds,
        estimated_volume_ml: scene.volume_mm3 / 1000.0,
        output_bytes: std::fs::metadata(output)?.len(),
    })
}

/// Slices every model in a GUI/project plate into one PWSZ job.
pub fn slice_project(project: &SceneProject, output: impl AsRef<Path>) -> Result<SliceReport> {
    let output = output.as_ref();
    let scene = prepare_project(project)?;
    let input = &project
        .models
        .first()
        .ok_or_else(|| Error::InvalidModel("scene project contains no models".into()))?
        .input;
    write_pwsz(
        output,
        input,
        &project.config,
        &scene.model,
        &scene.supports,
        &scene.layers,
        scene.volume_mm3,
    )?;
    slice_report(&scene, output)
}

/// Exports transformed triangles, generated supports, and every sliced contour
/// to the compact binary scene consumed by the Qt/OpenGL viewer.
pub fn export_viewer_scene(
    input: impl AsRef<Path>,
    output: impl AsRef<Path>,
    config: &SlicerConfig,
) -> Result<ViewerSceneReport> {
    let output = output.as_ref();
    let scene = prepare_scene(input.as_ref(), config)?;
    write_viewer_scene(output, config, &scene.model, &scene.supports, &scene.layers)?;
    Ok(ViewerSceneReport {
        model_triangle_count: scene.model.triangles.len(),
        support_triangle_count: scene.supports.triangles.len(),
        support_count: scene.support_count,
        layer_count: scene.layers.len() as u32,
        output_bytes: std::fs::metadata(output)?.len(),
    })
}

/// Exports a combined multi-model plate for sliced-layer inspection.
pub fn export_project_viewer_scene(
    project: &SceneProject,
    output: impl AsRef<Path>,
) -> Result<ViewerSceneReport> {
    let output = output.as_ref();
    let scene = prepare_project(project)?;
    write_viewer_scene(
        output,
        &project.config,
        &scene.model,
        &scene.supports,
        &scene.layers,
    )?;
    viewer_report(&scene, output)
}

fn slice_report(scene: &PreparedScene, output: &Path) -> Result<SliceReport> {
    Ok(SliceReport {
        triangle_count: scene.combined.triangles.len(),
        model_triangle_count: scene.model.triangles.len(),
        support_triangle_count: scene.supports.triangles.len(),
        support_count: scene.support_count,
        layer_count: scene.layers.len() as u32,
        bounds_mm: scene.bounds,
        estimated_volume_ml: scene.volume_mm3 / 1000.0,
        output_bytes: std::fs::metadata(output)?.len(),
    })
}

fn viewer_report(scene: &PreparedScene, output: &Path) -> Result<ViewerSceneReport> {
    Ok(ViewerSceneReport {
        model_triangle_count: scene.model.triangles.len(),
        support_triangle_count: scene.supports.triangles.len(),
        support_count: scene.support_count,
        layer_count: scene.layers.len() as u32,
        output_bytes: std::fs::metadata(output)?.len(),
    })
}

fn prepare_scene(input: &Path, config: &SlicerConfig) -> Result<PreparedScene> {
    prepare_project(&SceneProject {
        config: config.clone(),
        models: vec![SceneModelConfig {
            input: input.to_owned(),
            transform: config.transform.clone(),
            supports: config.supports.clone(),
        }],
    })
}

fn prepare_project(project: &SceneProject) -> Result<PreparedScene> {
    if project.models.is_empty() {
        return Err(Error::InvalidModel(
            "scene project contains no models".into(),
        ));
    }
    let mut model = Mesh {
        triangles: Vec::new(),
    };
    let mut supports = Mesh {
        triangles: Vec::new(),
    };
    let mut support_count = 0;
    let tessellation_error = tessellation_error_mm(&project.config);
    let stitch_tolerance = contour_tolerance_mm(&project.config);
    let topology_verified_cad = cfg!(anycubic_occt)
        && project.models.iter().all(|model| {
            model
                .input
                .extension()
                .and_then(|extension| extension.to_str())
                .is_some_and(|extension| {
                    matches!(
                        extension.to_ascii_lowercase().as_str(),
                        "step" | "stp" | "iges" | "igs"
                    )
                })
        });
    // Preserve CAD surface shape here. Broad contour healing belongs to the
    // 2D graph, while 3D welding is only for numerically coincident vertices.
    let weld_tolerance = (tessellation_error * 0.01).max(1.0e-7);
    for instance in &project.models {
        let mut instance_config = project.config.clone();
        instance_config.transform = instance.transform.clone();
        instance_config.supports = instance.supports.clone();
        validate_config(&instance_config)?;
        let mut instance_mesh = Mesh::load(
            &instance.input,
            &instance_config.gmsh_path,
            tessellation_error,
        )?;
        instance_mesh.apply_transform(&instance_config.transform)?;
        instance_mesh.repair_for_slicing(weld_tolerance)?;
        let generated = generate_supports(&instance_mesh, &instance_config.supports)?;
        support_count += generated.count;
        model.triangles.extend(instance_mesh.triangles);
        supports.triangles.extend(generated.mesh.triangles);
    }
    prepare_loaded_scene(
        model,
        supports,
        support_count,
        &project.config,
        stitch_tolerance,
        tessellation_error,
        topology_verified_cad,
    )
}

fn prepare_loaded_scene(
    model: Mesh,
    supports: Mesh,
    support_count: usize,
    config: &SlicerConfig,
    stitch_tolerance: f64,
    tessellation_error: f64,
    topology_verified_cad: bool,
) -> Result<PreparedScene> {
    let mut mesh = model.clone();
    mesh.triangles.extend_from_slice(&supports.triangles);
    let (minimum, maximum) = mesh.bounds();
    let size = [
        maximum.x - minimum.x,
        maximum.y - minimum.y,
        maximum.z - minimum.z,
    ];
    let generic_gap = stitch_tolerance * 5.0;
    // On a topologically closed B-Rep, neighbouring faces can sample their
    // common curved edge at different parametric positions. The along-edge
    // separation can approach a whole tessellation chord even though both
    // faces stay within the requested sub-pixel chordal error. Only models
    // explicitly validated as closed by OCCT receive this larger limit.
    let diagonal = (size[0].powi(2) + size[1].powi(2) + size[2].powi(2)).sqrt();
    let model_gap = if topology_verified_cad {
        (8.0 * diagonal * tessellation_error)
            .sqrt()
            .clamp(generic_gap, 3.0)
    } else {
        generic_gap
    };
    if size
        .iter()
        .zip(config.machine.build_volume_mm)
        .any(|(model, build)| *model > build + stitch_tolerance)
    {
        return Err(Error::InvalidModel(format!(
            "transformed model size {:.3} x {:.3} x {:.3} mm exceeds build volume {:.3} x {:.3} x {:.3} mm",
            size[0],
            size[1],
            size[2],
            config.machine.build_volume_mm[0],
            config.machine.build_volume_mm[1],
            config.machine.build_volume_mm[2]
        )));
    }
    let half_x = config.machine.build_volume_mm[0] * 0.5;
    let half_y = config.machine.build_volume_mm[1] * 0.5;
    if minimum.x < -half_x - stitch_tolerance
        || maximum.x > half_x + stitch_tolerance
        || minimum.y < -half_y - stitch_tolerance
        || maximum.y > half_y + stitch_tolerance
        || minimum.z < -stitch_tolerance
        || maximum.z > config.machine.build_volume_mm[2] + stitch_tolerance
    {
        return Err(Error::InvalidModel(format!(
            "transformed model bounds [{:.3}, {:.3}] x [{:.3}, {:.3}] x [{:.3}, {:.3}] mm lie outside the centered build volume",
            minimum.x, maximum.x, minimum.y, maximum.y, minimum.z, maximum.z
        )));
    }
    let layers = if supports.triangles.is_empty() {
        slice_mesh(
            &model,
            config.print.layer_height_mm,
            stitch_tolerance,
            model_gap,
        )?
    } else {
        let model_layers = slice_mesh(
            &model,
            config.print.layer_height_mm,
            stitch_tolerance,
            model_gap,
        )?;
        let support_layers = slice_mesh(
            &supports,
            config.print.layer_height_mm,
            stitch_tolerance,
            generic_gap,
        )?;
        geometry::merge_layers(&model_layers, &support_layers, config.print.layer_height_mm)
    };
    if let Some(layer) = layers
        .iter()
        .find(|layer| layer.segments.len() > config.machine.max_segments_per_layer)
    {
        return Err(Error::InvalidModel(format!(
            "layer {} contains {} segments, exceeding machine limit {}",
            layer.index,
            layer.segments.len(),
            config.machine.max_segments_per_layer
        )));
    }
    let volume_mm3 = layers
        .iter()
        .map(|layer| layer.area() * config.print.layer_height_mm)
        .sum::<f64>();
    Ok(PreparedScene {
        model,
        supports,
        combined: mesh,
        layers,
        support_count,
        bounds: [
            minimum.x, minimum.y, minimum.z, maximum.x, maximum.y, maximum.z,
        ],
        volume_mm3,
    })
}

/// Surface tessellation targets half an XY pixel, capped at the printer's
/// practical 10 µm Z resolution. This value is a chordal-error goal; Gmsh's
/// scale- and curvature-aware edge sizing is derived from it.
fn tessellation_error_mm(config: &SlicerConfig) -> f64 {
    let half_xy_pixel = config
        .machine
        .pixel_size_um
        .iter()
        .copied()
        .fold(f64::INFINITY, f64::min)
        / 2000.0;
    half_xy_pixel.clamp(0.001, 0.01)
}

/// Endpoints closer than the smallest LCD pixel are physically indistinguishable
/// on the target machine. Keeping the cap at 25 µm repairs
/// float/tessellation cracks without silently bridging macroscopic shell gaps.
fn contour_tolerance_mm(config: &SlicerConfig) -> f64 {
    let xy_pixel = config
        .machine
        .pixel_size_um
        .iter()
        .copied()
        .fold(f64::INFINITY, f64::min)
        / 1000.0;
    xy_pixel.clamp(1.0e-5, 0.025)
}

fn validate_config(config: &SlicerConfig) -> Result<()> {
    if config.hollow {
        return Err(Error::UnsupportedSlicerOption(
            "model hollowing is not implemented yet",
        ));
    }
    if config.xy_offset_mm != 0.0 {
        return Err(Error::UnsupportedSlicerOption(
            "XY contour offset is not implemented yet",
        ));
    }
    if config
        .machine
        .build_volume_mm
        .iter()
        .any(|value| !value.is_finite() || *value <= 0.0)
    {
        return Err(Error::InvalidModel(
            "machine build-volume dimensions must be finite and positive".into(),
        ));
    }
    if config.machine.resolution.contains(&0)
        || config
            .machine
            .pixel_size_um
            .iter()
            .any(|value| !value.is_finite() || *value <= 0.0)
        || config.machine.max_segments_per_layer == 0
    {
        return Err(Error::InvalidModel(
            "machine resolution, pixel size, and segment limit must be positive".into(),
        ));
    }
    let supports = &config.supports;
    for (name, value, allow_zero) in [
        ("support overhang angle", supports.overhang_angle_deg, false),
        ("support spacing", supports.spacing_mm, false),
        ("support tip diameter", supports.tip_diameter_mm, false),
        ("support trunk diameter", supports.trunk_diameter_mm, false),
        ("support base diameter", supports.base_diameter_mm, false),
        ("support tip height", supports.tip_height_mm, false),
        ("support base height", supports.base_height_mm, true),
        ("support penetration", supports.penetration_mm, true),
        ("support minimum height", supports.min_height_mm, true),
        ("support maximum height", supports.max_height_mm, false),
    ] {
        if !value.is_finite() || value < 0.0 || (!allow_zero && value == 0.0) {
            return Err(Error::InvalidModel(format!("invalid {name}: {value}")));
        }
    }
    if supports.overhang_angle_deg <= 1.0 || supports.overhang_angle_deg >= 89.0 {
        return Err(Error::InvalidModel(
            "support overhang angle must be between 1 and 89 degrees".into(),
        ));
    }
    if supports.tip_diameter_mm > supports.trunk_diameter_mm
        || supports.trunk_diameter_mm > supports.base_diameter_mm
    {
        return Err(Error::InvalidModel(
            "support diameters must satisfy tip <= trunk <= base".into(),
        ));
    }
    if supports.min_height_mm > supports.max_height_mm {
        return Err(Error::InvalidModel(
            "support minimum height must not exceed maximum height".into(),
        ));
    }
    let minimum_segments = if supports.shape == SupportShape::Square {
        3
    } else {
        6
    };
    if !(minimum_segments..=128).contains(&supports.radial_segments) {
        return Err(Error::InvalidModel(format!(
            "support radial segment count must be between {minimum_segments} and 128"
        )));
    }
    let print = &config.print;
    for (name, value, allow_zero) in [
        ("layer height", print.layer_height_mm, false),
        ("bottom exposure", print.bottom_exposure_s, true),
        ("normal exposure", print.exposure_s, true),
        ("lift height", print.lift_height_mm, true),
        ("lift speed", print.lift_speed_mm_s, false),
        ("off time", print.off_time_s, true),
    ] {
        if !value.is_finite() || value < 0.0 || (!allow_zero && value == 0.0) {
            return Err(Error::InvalidModel(format!("invalid {name}: {value}")));
        }
    }
    Ok(())
}
