use std::{
    fs::File,
    io::{BufWriter, Cursor, Seek, Write},
    path::Path,
};

use crc32fast::Hasher;
use flate2::{Compression, write::ZlibEncoder};
use rayon::prelude::*;
use serde_json::{Value, json};
use zip::{CompressionMethod, ZipWriter, write::SimpleFileOptions};

use crate::{Error, Result};

use super::{
    SlicerConfig,
    geometry::LayerSlice,
    mesh::{Mesh, Vec3},
    render::{RenderedImage, render_previews},
};

const OUTER_MAGIC: u32 = 0x003d_3d7b; // "{==\0"
const OUTER_FOOTER: u32 = 0x007d_3d3d; // "==}\0"
const INNER_MAGIC: u32 = 0x002d_2d5b; // "[--\0"
const INNER_FOOTER: u32 = 0x005d_2d2d; // "--]\0"
const SCENE_LAYERS_MAGIC: u32 = 0x2d2d_2d3c; // "<---"
const SCENE_LAYERS_FOOTER: u32 = 0x3e2d_2d2d; // "--->"

pub(crate) fn write_pwsz(
    output: &Path,
    input: &Path,
    config: &SlicerConfig,
    model: &Mesh,
    supports: &Mesh,
    layers: &[LayerSlice],
    volume_mm3: f64,
) -> Result<()> {
    let mut timings = super::timing::Timings::new("output");
    let previews = render_previews(model, supports)?;
    timings.lap("preview rendering");
    write_pwsz_with_previews(
        output, input, config, model, supports, layers, volume_mm3, &previews, false,
    )
}

#[allow(clippy::too_many_arguments)]
pub(crate) fn write_pwsz_with_previews(
    output: &Path,
    input: &Path,
    config: &SlicerConfig,
    model: &Mesh,
    supports: &Mesh,
    layers: &[LayerSlice],
    volume_mm3: f64,
    previews: &[RenderedImage],
    pcb_mode: bool,
) -> Result<()> {
    let mut timings = super::timing::Timings::new("PWSZ");
    let mut archive = ZipWriter::new(BufWriter::with_capacity(256 * 1024, File::create(output)?));
    let options = SimpleFileOptions::default().compression_method(CompressionMethod::Deflated);
    write_json(
        &mut archive,
        "anycubic_photon_resins.pwsp",
        &machine_profile(config, pcb_mode)?,
        options,
    )?;
    write_json(
        &mut archive,
        "print_info.json",
        &print_info(config, layers, volume_mm3),
        options,
    )?;
    write_json(
        &mut archive,
        "software_info.conf",
        &json!({
            "mark": "ANYCUBIC-PC",
            "opengl": "independent-vector-slicer",
            "os": std::env::consts::OS,
            "version": env!("CARGO_PKG_VERSION")
        }),
        options,
    )?;

    for (index, rendered) in previews.iter().enumerate() {
        let preview = encode_png(rendered.width, rendered.height, &rendered.rgba)?;
        write_member(
            &mut archive,
            &format!("preview_images/preview_{index}.png"),
            &preview,
            options,
        )?;
    }

    write_json(
        &mut archive,
        "layers_controller.conf",
        &layers_controller(config, layers),
        options,
    )?;
    write_json(
        &mut archive,
        "lcd_function.json",
        &lcd_function(input, config),
        options,
    )?;
    timings.lap("metadata and PNGs");
    // Compress bounded batches on the shared workers. Merge their already
    // compressed ZIP entries in layer order, without a second deflate pass.
    // The layer codec, compression method and level are unchanged.
    for batch in layers.chunks(128) {
        let chunks = super::parallel::install(|| {
            batch
                .par_chunks(16)
                .map(|chunk| -> Result<_> {
                    let mut compressed = ZipWriter::new(Cursor::new(Vec::new()));
                    for layer in chunk {
                        write_member(
                            &mut compressed,
                            &format!("layer_images/layer_{}.pwszImg", layer.index),
                            &encode_layer(layer),
                            options,
                        )?;
                    }
                    Ok(compressed.finish_into_readable()?)
                })
                .collect::<Vec<_>>()
        })?;
        for compressed in chunks {
            archive.merge_archive(compressed?)?;
        }
    }
    timings.lap("encode and compress layers");
    write_member(
        &mut archive,
        "calc_layer_volumes.data",
        &encode_layer_volumes(config, layers),
        options,
    )?;
    write_member(
        &mut archive,
        "scene.slice",
        &encode_scene(model, supports, layers),
        options,
    )?;
    archive.finish()?.flush()?;
    timings.lap("scene and finish");
    Ok(())
}

fn machine_profile(config: &SlicerConfig, pcb_mode: bool) -> Result<Value> {
    let machine = &config.machine;
    let print = &config.print;
    // Cloud's slicing engine rejects a syntactically valid but abbreviated
    // profile with ENGINE_ABNORMAL.  Preserve the complete M7 Pro factory
    // profile recovered from a Workshop 4.1.8 job and update only values that
    // belong to this slice.  In particular, the resin catalogue and firmware
    // timing tables are not optional even though the printer consumes neither
    // of them directly from an already-rasterised PWSZ.
    let mut profile: Value =
        serde_json::from_str(include_str!("../../assets/m7_pro_factory_profile.pwsp"))?;

    set_profile_value(&mut profile, "/machine_type/name", json!(machine.name))?;
    set_profile_value(
        &mut profile,
        "/machine_type/res_x",
        json!(machine.resolution[0]),
    )?;
    set_profile_value(
        &mut profile,
        "/machine_type/res_y",
        json!(machine.resolution[1]),
    )?;
    set_profile_value(
        &mut profile,
        "/machine_type/xy_pixel",
        json!(machine.pixel_size_um[0]),
    )?;
    set_profile_value(
        &mut profile,
        "/machine_type/xy_pixel_y",
        json!(machine.pixel_size_um[1]),
    )?;
    set_profile_value(
        &mut profile,
        "/machine_type/print_xsize",
        json!(machine.build_volume_mm[0]),
    )?;
    set_profile_value(
        &mut profile,
        "/machine_type/print_ysize",
        json!(machine.build_volume_mm[1]),
    )?;
    set_profile_value(
        &mut profile,
        "/machine_type/print_zsize",
        json!(machine.build_volume_mm[2]),
    )?;
    set_profile_value(
        &mut profile,
        "/machine_type/child_screen",
        json!([{
            "x": 0,
            "y": 0,
            "width": machine.resolution[0],
            "height": machine.resolution[1]
        }]),
    )?;
    set_profile_value(
        &mut profile,
        "/machine_type/raster_segments_capacity",
        json!(machine.max_segments_per_layer),
    )?;
    set_profile_value(&mut profile, "/machine_extern/alias", json!(machine.name))?;

    let property = "/machine_extern/user_resins/0/property";
    set_profile_value(
        &mut profile,
        &format!("{property}/currency"),
        json!(print.currency),
    )?;
    set_profile_value(
        &mut profile,
        &format!("{property}/price"),
        json!(print.resin_price),
    )?;
    set_profile_value(
        &mut profile,
        &format!("{property}/volume"),
        json!(print.resin_volume_ml),
    )?;
    set_profile_value(
        &mut profile,
        &format!("{property}/density"),
        json!(print.resin_density_g_ml),
    )?;

    let slice = "/machine_extern/user_resins/0/slicepara";
    for (field, value) in [
        ("bott_layers", json!(print.bottom_layers)),
        ("bott_time", json!(print.bottom_exposure_s)),
        ("bott_time_dual", json!(print.bottom_exposure_s)),
        ("bott_off_time", json!(print.off_time_s)),
        ("bott_wait_before_lift", json!(0.0)),
        ("bott_wait_after_lift", json!(0.0)),
        ("exposure_time", json!(print.exposure_s)),
        ("off_time", json!(print.off_time_s)),
        ("zdown_speed", json!(print.lift_speed_mm_s)),
        ("zthick", json!(print.layer_height_mm)),
        ("zup_height", json!(print.lift_height_mm)),
        ("zup_speed", json!(print.lift_speed_mm_s)),
    ] {
        set_profile_value(&mut profile, &format!("{slice}/{field}"), value)?;
    }

    if pcb_mode {
        // `intelli_mode` is the slice-profile counterpart of intelligent
        // release. Keep normal resin jobs unchanged, but do not ask firmware
        // to adapt release behaviour for a one-layer PCB mask.
        set_profile_value(
            &mut profile,
            "/machine_extern/user_resins/0/slice_extpara/intelli_mode",
            json!(0),
        )?;
    }

    Ok(profile)
}

fn set_profile_value(profile: &mut Value, pointer: &str, value: Value) -> Result<()> {
    let target = profile.pointer_mut(pointer).ok_or(Error::Protocol(
        "bundled M7 Pro profile is missing a required field",
    ))?;
    *target = value;
    Ok(())
}

fn print_info(config: &SlicerConfig, layers: &[LayerSlice], volume_mm3: f64) -> Value {
    let volume = volume_mm3 / 1000.0;
    let weight = volume * config.print.resin_density_g_ml;
    let cost = if config.print.resin_volume_ml > 0.0 {
        volume * config.print.resin_price / config.print.resin_volume_ml
    } else {
        0.0
    };
    let print_time = estimated_print_time(config, layers.len());
    json!({
        "cost":cost, "currency":config.print.currency,
        "model_layers_count":layers.len(), "print_time":print_time,
        "sub_estimated_infos":[{
            "estimated_cost":cost, "estimated_cost_currency":config.print.currency,
            "estimated_time":print_time, "estimated_volume":volume,
            "estimated_weight":weight, "model_layers_count":layers.len()
        }],
        "volume":volume, "weight":weight
    })
}

fn estimated_print_time(config: &SlicerConfig, layer_count: usize) -> u64 {
    let bottom = config.print.bottom_layers.min(layer_count as u32) as usize;
    let exposure = bottom as f64 * config.print.bottom_exposure_s
        + (layer_count - bottom) as f64 * config.print.exposure_s;
    let movement = layer_count as f64
        * (2.0 * config.print.lift_height_mm / config.print.lift_speed_mm_s
            + config.print.off_time_s);
    (exposure + movement).round() as u64
}

fn layers_controller(config: &SlicerConfig, layers: &[LayerSlice]) -> Value {
    let parameters = layers
        .iter()
        .map(|layer| {
            json!({
                "exposure_time": if layer.index < config.print.bottom_layers {
                    config.print.bottom_exposure_s
                } else {
                    config.print.exposure_s
                },
                "layer_index":layer.index,
                "layer_minheight":layer.index as f64 * config.print.layer_height_mm,
                "layer_thickness":config.print.layer_height_mm,
                "zup_height":config.print.lift_height_mm,
                "zup_speed":config.print.lift_speed_mm_s
            })
        })
        .collect::<Vec<_>>();
    json!({"count":layers.len(), "paras":parameters})
}

fn lcd_function(input: &Path, config: &SlicerConfig) -> Value {
    let name = input
        .file_name()
        .and_then(|name| name.to_str())
        .unwrap_or("model");
    json!({
        "models_processed_info": {
            "models":[{
                "auto_support":u8::from(config.supports.enabled && config.supports.auto_generate),
                "hollow":u8::from(config.hollow), "makeronline_sourceid":-1,
                "manual_support":u8::from(!config.supports.manual_points.is_empty()),
                "name":name, "punch":0, "source_from":0
            }],
            "scene_models_from":0, "slice_paras_process":1,
            "software_version":concat!("anycubicstudio_", env!("CARGO_PKG_VERSION"))
        },
        "rerf_function": {
            "enable":false, "model_name":"", "model_type":1,
            "partition_exposure_array":[], "partition_num":0
        }
    })
}

pub(crate) fn encode_layer(layer: &LayerSlice) -> Vec<u8> {
    let mut output = Vec::with_capacity(56 + layer.segments.len() * 17);
    push_u32(&mut output, OUTER_MAGIC);
    push_f32(&mut output, layer.signed_twice_area as f32);
    for bound in layer.bounds {
        push_f32(&mut output, bound as f32);
    }
    push_u32(&mut output, 0); // auxiliary index vector count
    push_u32(&mut output, layer.contours.len() as u32);
    push_u32(&mut output, INNER_MAGIC);
    push_u32(&mut output, 0); // inner reserved field
    push_u32(&mut output, layer.segments.len() as u32);
    push_u32(&mut output, 1); // binary segment encoding
    for segment in &layer.segments {
        push_f32(&mut output, segment.start.x as f32);
        push_f32(&mut output, segment.start.y as f32);
        push_f32(&mut output, segment.end.x as f32);
        push_f32(&mut output, segment.end.y as f32);
        output.push(u8::from(segment.direction));
    }
    push_u32(&mut output, INNER_FOOTER);
    push_u32(&mut output, OUTER_FOOTER);
    output
}

fn encode_scene(model: &Mesh, supports: &Mesh, layers: &[LayerSlice]) -> Vec<u8> {
    let mut mesh_points = model.triangles.iter().flat_map(|triangle| triangle.0);
    let mesh_bounds = mesh_points.next().map(|first| {
        mesh_points.fold((first, first), |(minimum, maximum), point| {
            (
                Vec3 {
                    x: minimum.x.min(point.x),
                    y: minimum.y.min(point.y),
                    z: minimum.z.min(point.z),
                },
                Vec3 {
                    x: maximum.x.max(point.x),
                    y: maximum.y.max(point.y),
                    z: maximum.z.max(point.z),
                },
            )
        })
    });
    let (mut minimum, mut maximum) = mesh_bounds.unwrap_or_else(|| {
        let bounds = layers
            .iter()
            .fold(None, |combined: Option<[f64; 4]>, layer| {
                Some(match combined {
                    None => layer.bounds,
                    Some(current) => [
                        current[0].min(layer.bounds[0]),
                        current[1].min(layer.bounds[1]),
                        current[2].max(layer.bounds[2]),
                        current[3].max(layer.bounds[3]),
                    ],
                })
            })
            .unwrap_or([0.0; 4]);
        (
            Vec3 {
                x: bounds[0],
                y: bounds[1],
                z: 0.0,
            },
            Vec3 {
                x: bounds[2],
                y: bounds[3],
                z: layers.last().map_or(0.0, |layer| layer.z),
            },
        )
    });
    if !supports.triangles.is_empty() {
        let (support_minimum, support_maximum) = supports.bounds();
        minimum.x = minimum.x.min(support_minimum.x);
        minimum.y = minimum.y.min(support_minimum.y);
        minimum.z = minimum.z.min(support_minimum.z);
        maximum.x = maximum.x.max(support_maximum.x);
        maximum.y = maximum.y.max(support_maximum.y);
        maximum.z = maximum.z.max(support_maximum.z);
    }
    let mut output = Vec::with_capacity(400 + layers.len() * 64);
    output.extend_from_slice(b"ANYCUBIC-PWSZ\0\0\0");
    let mut description = [0_u8; 64];
    let text = b"AnycubicPhotonWorkshop Exports";
    description[..text.len()].copy_from_slice(text);
    output.extend_from_slice(&description);
    push_u32(&mut output, 3);
    push_u32(&mut output, 3);
    push_u32(&mut output, 0);
    push_u32(&mut output, 0);
    push_f32(&mut output, 1.0);
    push_u32(&mut output, layers.len() as u32);
    for coordinate in [
        minimum.x, minimum.y, minimum.z, maximum.x, maximum.y, maximum.z,
    ] {
        push_f32(&mut output, coordinate as f32);
    }
    for _ in 0..65 {
        push_u32(&mut output, 0);
    }
    debug_assert_eq!(output.len(), 388);
    push_u32(&mut output, SCENE_LAYERS_MAGIC);
    push_u32(&mut output, layers.len() as u32);
    for layer in layers {
        push_f32(&mut output, layer.z as f32);
        push_f32(&mut output, layer.area() as f32);
        for bound in layer.bounds {
            push_f32(&mut output, bound as f32);
        }
        push_u32(&mut output, layer.contours.len() as u32);
        for _ in 0..9 {
            push_u32(&mut output, 0);
        }
    }
    push_u32(&mut output, SCENE_LAYERS_FOOTER);
    output
}

/// The container layout is confirmed, while the M7 firmware's adaptive
/// sampling policy is not. We emit deterministic four-layer volume groups.
fn encode_layer_volumes(config: &SlicerConfig, layers: &[LayerSlice]) -> Vec<u8> {
    let groups = layers.chunks(4).collect::<Vec<_>>();
    let mut output = Vec::with_capacity(16 + groups.len() * 48);
    push_u32(&mut output, 0x2025_1024);
    push_f32(&mut output, 0.2);
    push_u32(&mut output, 1);
    push_u32(&mut output, groups.len() as u32);
    for group in groups {
        push_u32(&mut output, group[0].index);
        push_f32(
            &mut output,
            (group[0].index as f64 * config.print.layer_height_mm) as f32,
        );
        push_f32(
            &mut output,
            (group.len() as f64 * config.print.layer_height_mm) as f32,
        );
        for _ in 0..8 {
            push_u32(&mut output, 0);
        }
        let volume = group
            .iter()
            .map(|layer| layer.area() * config.print.layer_height_mm)
            .sum::<f64>();
        push_f32(&mut output, volume as f32);
    }
    output
}

fn write_json<W: Write + Seek>(
    archive: &mut ZipWriter<W>,
    name: &str,
    value: &Value,
    options: SimpleFileOptions,
) -> Result<()> {
    write_member(archive, name, &serde_json::to_vec_pretty(value)?, options)
}

fn write_member<W: Write + Seek>(
    archive: &mut ZipWriter<W>,
    name: &str,
    bytes: &[u8],
    options: SimpleFileOptions,
) -> Result<()> {
    archive.start_file(name, options)?;
    archive.write_all(bytes)?;
    Ok(())
}

fn push_u32(output: &mut Vec<u8>, value: u32) {
    output.extend_from_slice(&value.to_le_bytes());
}

fn push_f32(output: &mut Vec<u8>, value: f32) {
    output.extend_from_slice(&value.to_le_bytes());
}

fn encode_png(width: u32, height: u32, rgba: &[u8]) -> Result<Vec<u8>> {
    debug_assert_eq!(rgba.len(), width as usize * height as usize * 4);
    let mut pixels = Vec::with_capacity((height as usize) * (1 + width as usize * 4));
    for scanline in rgba.chunks_exact(width as usize * 4) {
        pixels.push(0); // PNG scanline filter: None
        pixels.extend_from_slice(scanline);
    }
    let mut encoder = ZlibEncoder::new(Vec::new(), Compression::fast());
    encoder.write_all(&pixels)?;
    let compressed = encoder.finish()?;

    let mut png = b"\x89PNG\r\n\x1a\n".to_vec();
    let mut header = Vec::with_capacity(13);
    header.extend_from_slice(&width.to_be_bytes());
    header.extend_from_slice(&height.to_be_bytes());
    header.extend_from_slice(&[8, 6, 0, 0, 0]); // 8-bit RGBA
    png_chunk(&mut png, b"IHDR", &header);
    png_chunk(&mut png, b"IDAT", &compressed);
    png_chunk(&mut png, b"IEND", &[]);
    Ok(png)
}

fn png_chunk(output: &mut Vec<u8>, name: &[u8; 4], data: &[u8]) {
    output.extend_from_slice(&(data.len() as u32).to_be_bytes());
    output.extend_from_slice(name);
    output.extend_from_slice(data);
    let mut crc = Hasher::new();
    crc.update(name);
    crc.update(data);
    output.extend_from_slice(&crc.finalize().to_be_bytes());
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::slicer::geometry::{EncodedSegment, Vec2};

    #[test]
    fn pwsz_layer_matches_recovered_binary_layout() {
        let layer = LayerSlice {
            index: 0,
            z: 0.025,
            contours: vec![vec![
                Vec2 { x: 0.0, y: 0.0 },
                Vec2 { x: 1.0, y: 0.0 },
                Vec2 { x: 0.0, y: 1.0 },
            ]],
            segments: vec![EncodedSegment {
                start: Vec2 { x: 0.0, y: 0.0 },
                end: Vec2 { x: 1.0, y: 0.0 },
                direction: true,
            }],
            signed_twice_area: 1.0,
            bounds: [0.0, 0.0, 1.0, 1.0],
        };
        let encoded = encode_layer(&layer);
        assert_eq!(encoded.len(), 56 + 17);
        assert_eq!(&encoded[0..4], &OUTER_MAGIC.to_le_bytes());
        assert_eq!(&encoded[32..36], &INNER_MAGIC.to_le_bytes());
        assert_eq!(
            &encoded[encoded.len() - 8..encoded.len() - 4],
            &INNER_FOOTER.to_le_bytes()
        );
        assert_eq!(&encoded[encoded.len() - 4..], &OUTER_FOOTER.to_le_bytes());
    }
}
