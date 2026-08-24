//! Independent mesh-to-PWSZ slicer.
//!
//! The PWSZ container and vector layer encoder are based on the observable
//! Photon Workshop 4.1.8 file contract. STEP/IGES tessellation is delegated to
//! Gmsh; STL and the mesh subset of 3MF are decoded directly by this crate.

mod ffi;
mod geometry;
mod mesh;
mod writer;

use std::path::{Path, PathBuf};

use serde::{Deserialize, Serialize};

use crate::{Error, Result};

use self::{geometry::slice_mesh, mesh::Mesh, writer::write_pwsz};

pub use ffi::AnycubicSlicerConfig;

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

#[derive(Debug, Clone, Default, Serialize, Deserialize)]
pub struct SupportConfig {
    /// Reserved API surface. Setting this to true currently returns an error;
    /// unsupported options are never silently ignored.
    pub enabled: bool,
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
    pub layer_count: u32,
    pub bounds_mm: [f64; 6],
    pub estimated_volume_ml: f64,
    pub output_bytes: u64,
}

/// Slices one STL, 3MF, STEP/STP or IGES model into a PWSZ file.
pub fn slice_file(
    input: impl AsRef<Path>,
    output: impl AsRef<Path>,
    config: &SlicerConfig,
) -> Result<SliceReport> {
    validate_config(config)?;
    let input = input.as_ref();
    let output = output.as_ref();
    let mut mesh = Mesh::load(input, &config.gmsh_path)?;
    mesh.apply_transform(&config.transform)?;
    let (minimum, maximum) = mesh.bounds();
    let size = [
        maximum.x - minimum.x,
        maximum.y - minimum.y,
        maximum.z - minimum.z,
    ];
    if size
        .iter()
        .zip(config.machine.build_volume_mm)
        .any(|(model, build)| *model > build + 1.0e-6)
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
    if minimum.x < -half_x - 1.0e-6
        || maximum.x > half_x + 1.0e-6
        || minimum.y < -half_y - 1.0e-6
        || maximum.y > half_y + 1.0e-6
        || minimum.z < -1.0e-6
        || maximum.z > config.machine.build_volume_mm[2] + 1.0e-6
    {
        return Err(Error::InvalidModel(format!(
            "transformed model bounds [{:.3}, {:.3}] x [{:.3}, {:.3}] x [{:.3}, {:.3}] mm lie outside the centered build volume",
            minimum.x, maximum.x, minimum.y, maximum.y, minimum.z, maximum.z
        )));
    }
    let layers = slice_mesh(&mesh, config.print.layer_height_mm)?;
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
    write_pwsz(output, input, config, &mesh, &layers, volume_mm3)?;
    let output_bytes = std::fs::metadata(output)?.len();
    Ok(SliceReport {
        triangle_count: mesh.triangles.len(),
        layer_count: layers.len() as u32,
        bounds_mm: [
            minimum.x, minimum.y, minimum.z, maximum.x, maximum.y, maximum.z,
        ],
        estimated_volume_ml: volume_mm3 / 1000.0,
        output_bytes,
    })
}

fn validate_config(config: &SlicerConfig) -> Result<()> {
    if config.supports.enabled {
        return Err(Error::UnsupportedSlicerOption(
            "automatic supports are not implemented yet",
        ));
    }
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
