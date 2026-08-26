use std::{fs::File, io::Write, path::Path};

use crate::Result;

use super::{SlicerConfig, geometry::LayerSlice, mesh::Mesh};

/// Binary viewer scene v1:
/// magic[8], version, build volume, triangle/layer counts, model triangles,
/// support triangles, then Z + polygon point arrays for every final layer.
pub(crate) fn write_viewer_scene(
    output: &Path,
    config: &SlicerConfig,
    model: &Mesh,
    supports: &Mesh,
    layers: &[LayerSlice],
) -> Result<()> {
    let mut file = File::create(output)?;
    file.write_all(b"ACVSCN1\0")?;
    write_u32(&mut file, 1)?;
    for size in config.machine.build_volume_mm {
        write_f32(&mut file, size as f32)?;
    }
    write_u32(&mut file, model.triangles.len() as u32)?;
    write_u32(&mut file, supports.triangles.len() as u32)?;
    write_u32(&mut file, layers.len() as u32)?;
    for triangle in model.triangles.iter().chain(&supports.triangles) {
        for point in triangle.0 {
            write_f32(&mut file, point.x as f32)?;
            write_f32(&mut file, point.y as f32)?;
            write_f32(&mut file, point.z as f32)?;
        }
    }
    for layer in layers {
        write_f32(&mut file, layer.z as f32)?;
        write_u32(&mut file, layer.contours.len() as u32)?;
        for contour in &layer.contours {
            write_u32(&mut file, contour.len() as u32)?;
            for point in contour {
                write_f32(&mut file, point.x as f32)?;
                write_f32(&mut file, point.y as f32)?;
            }
        }
    }
    file.flush()?;
    Ok(())
}

fn write_u32(file: &mut File, value: u32) -> std::io::Result<()> {
    file.write_all(&value.to_le_bytes())
}

fn write_f32(file: &mut File, value: f32) -> std::io::Result<()> {
    file.write_all(&value.to_le_bytes())
}
