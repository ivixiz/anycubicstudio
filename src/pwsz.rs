//! Reader for the ZIP-based PWSZ print-job container.
//!
//! This module intentionally reads only the parts proved by a real PWSZ file
//! and the 4.1.8 writer/reader code.  `pwszImg` payloads are exposed as bytes:
//! their 48-byte header is observable, but the following segment stream has
//! not yet been completely reversed and must not be mistaken for a bitmap.

use std::{
    fs::File,
    io::{Read, Seek},
    path::Path,
};

use serde::Deserialize;
use serde_json::{Map, Value};
use zip::ZipArchive;

use crate::error::{Error, Result};

const MACHINE_PROFILE: &str = "anycubic_photon_resins.pwsp";
const PRINT_INFO: &str = "print_info.json";
const LAYERS: &str = "layers_controller.conf";
const SOFTWARE_INFO: &str = "software_info.conf";
const LCD_FUNCTION: &str = "lcd_function.json";
const LAYER_PREFIX: &str = "layer_images/layer_";
const PREVIEW_PREFIX: &str = "preview_images/preview_";

/// A PWSZ job held in an ordinary ZIP archive.
pub struct PwszArchive<R: Read + Seek> {
    archive: ZipArchive<R>,
    metadata: PwszMetadata,
}

impl PwszArchive<File> {
    /// Opens a PWSZ file from disk. No print or network operation is made.
    pub fn open(path: impl AsRef<Path>) -> Result<Self> {
        Self::from_reader(File::open(path)?)
    }
}

impl<R: Read + Seek> PwszArchive<R> {
    /// Opens a PWSZ archive from any seekable source, such as an in-memory
    /// cursor. The source must be a valid ZIP archive.
    pub fn from_reader(reader: R) -> Result<Self> {
        let mut archive = ZipArchive::new(reader)?;
        let metadata = PwszMetadata::read_from(&mut archive)?;
        Ok(Self { archive, metadata })
    }

    pub fn metadata(&self) -> &PwszMetadata {
        &self.metadata
    }

    /// Reads the exact stored bytes of a layer image. The returned bytes are
    /// the *uncompressed ZIP member*; they are still in native `pwszImg`
    /// representation and are not a PNG or raw LCD bitmap.
    pub fn read_layer(&mut self, index: u32) -> Result<PwszLayerImage> {
        let path = format!(
            "{LAYER_PREFIX}{index}.{}",
            self.metadata.machine.key_image_format
        );
        let mut file = self.archive.by_name(&path)?;
        let mut bytes = Vec::with_capacity(file.size() as usize);
        file.read_to_end(&mut bytes)?;
        Ok(PwszLayerImage { index, bytes })
    }

    /// Reads a preview image. In the observed format previews are regular PNG
    /// files, but the method deliberately returns their original bytes.
    pub fn read_preview(&mut self, index: u32) -> Result<Vec<u8>> {
        let path = format!("{PREVIEW_PREFIX}{index}.png");
        let mut file = self.archive.by_name(&path)?;
        let mut bytes = Vec::with_capacity(file.size() as usize);
        file.read_to_end(&mut bytes)?;
        Ok(bytes)
    }
}

/// Metadata and index collected from the PWSZ members.
#[derive(Debug, Clone)]
pub struct PwszMetadata {
    pub machine: MachineType,
    pub machine_extern: MachineExtern,
    pub print_info: PrintInfo,
    pub layers: LayerController,
    /// `software_info.conf` is JSON in the observed PWSZ sample.
    pub software_info: Value,
    /// Optional because old files may precede this writer member.
    pub lcd_function: Option<Value>,
    /// All ZIP member names in central-directory order.
    pub entries: Vec<ArchiveEntry>,
    pub layer_indices: Vec<u32>,
    pub preview_indices: Vec<u32>,
}

impl PwszMetadata {
    fn read_from<R: Read + Seek>(archive: &mut ZipArchive<R>) -> Result<Self> {
        let profile: MachineProfile = read_json(archive, MACHINE_PROFILE)?;
        let print_info = read_json(archive, PRINT_INFO)?;
        let layers: LayerController = read_json(archive, LAYERS)?;
        let software_info = read_json(archive, SOFTWARE_INFO)?;
        let lcd_function = read_json_optional(archive, LCD_FUNCTION)?;

        let entries = (0..archive.len())
            .map(|index| {
                let file = archive.by_index(index)?;
                Ok(ArchiveEntry {
                    name: file.name().to_owned(),
                    uncompressed_size: file.size(),
                    compressed_size: file.compressed_size(),
                })
            })
            .collect::<Result<Vec<_>>>()?;

        let extension = format!(".{}", profile.machine_type.key_image_format);
        let mut layer_indices = entries
            .iter()
            .filter_map(|entry| parse_index(&entry.name, LAYER_PREFIX, &extension))
            .collect::<Vec<_>>();
        layer_indices.sort_unstable();

        let mut preview_indices = entries
            .iter()
            .filter_map(|entry| parse_index(&entry.name, PREVIEW_PREFIX, ".png"))
            .collect::<Vec<_>>();
        preview_indices.sort_unstable();

        if layers.count != layers.paras.len() as u32 {
            return Err(Error::Protocol(
                "layers_controller count does not match its parameter array",
            ));
        }
        if layers.count != layer_indices.len() as u32 {
            return Err(Error::Protocol(
                "layers_controller count does not match layer image entries",
            ));
        }

        Ok(Self {
            machine: profile.machine_type,
            machine_extern: profile.machine_extern,
            print_info,
            layers,
            software_info,
            lcd_function,
            entries,
            layer_indices,
            preview_indices,
        })
    }
}

/// ZIP central-directory information; this makes no claim about a member's
/// internal data format.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ArchiveEntry {
    pub name: String,
    pub uncompressed_size: u64,
    pub compressed_size: u64,
}

#[derive(Debug, Clone, Deserialize)]
struct MachineProfile {
    machine_type: MachineType,
    machine_extern: MachineExtern,
}

/// LCD/build-volume data read from `machine_type`.
#[derive(Debug, Clone, Deserialize)]
pub struct MachineType {
    pub name: String,
    pub key_suffix: String,
    pub key_image_format: String,
    pub res_x: u32,
    pub res_y: u32,
    pub xy_pixel: f64,
    pub xy_pixel_y: f64,
    pub print_xsize: f64,
    pub print_ysize: f64,
    pub print_zsize: f64,
    #[serde(default)]
    pub max_file_version: Option<u32>,
    #[serde(default)]
    pub raster_segments_capacity: Option<u32>,
    #[serde(default)]
    pub raster_antialiasing: Option<u32>,
    #[serde(flatten)]
    pub extra: Map<String, Value>,
}

/// Printer-specific resin and firmware-calculation data. Unknown fields are
/// deliberately retained instead of being discarded during reverse engineering.
#[derive(Debug, Clone, Deserialize)]
pub struct MachineExtern {
    pub alias: String,
    #[serde(default)]
    pub active_resins: Vec<String>,
    #[serde(default)]
    pub factory_resins: Vec<Value>,
    #[serde(default)]
    pub user_resins: Vec<Value>,
    #[serde(flatten)]
    pub extra: Map<String, Value>,
}

#[derive(Debug, Clone, Deserialize)]
pub struct PrintInfo {
    #[serde(default)]
    pub cost: Option<f64>,
    #[serde(default)]
    pub currency: Option<String>,
    #[serde(default)]
    pub model_layers_count: Option<u32>,
    #[serde(default)]
    pub print_time: Option<u64>,
    #[serde(default)]
    pub volume: Option<f64>,
    #[serde(default)]
    pub weight: Option<f64>,
    #[serde(default)]
    pub sub_estimated_infos: Vec<EstimatedInfo>,
    #[serde(flatten)]
    pub extra: Map<String, Value>,
}

#[derive(Debug, Clone, Deserialize)]
pub struct EstimatedInfo {
    #[serde(default)]
    pub estimated_cost: Option<f64>,
    #[serde(default)]
    pub estimated_cost_currency: Option<String>,
    #[serde(default)]
    pub estimated_time: Option<u64>,
    #[serde(default)]
    pub estimated_volume: Option<f64>,
    #[serde(default)]
    pub estimated_weight: Option<f64>,
    #[serde(default)]
    pub model_layers_count: Option<u32>,
    #[serde(flatten)]
    pub extra: Map<String, Value>,
}

#[derive(Debug, Clone, Deserialize)]
pub struct LayerController {
    pub count: u32,
    pub paras: Vec<LayerParameters>,
    #[serde(flatten)]
    pub extra: Map<String, Value>,
}

/// Per-layer motion and exposure values as written in `layers_controller.conf`.
#[derive(Debug, Clone, Deserialize)]
pub struct LayerParameters {
    pub layer_index: u32,
    pub layer_minheight: f64,
    pub layer_thickness: f64,
    pub exposure_time: f64,
    #[serde(default)]
    pub zup_height: Option<f64>,
    #[serde(default)]
    pub zup_speed: Option<f64>,
    #[serde(flatten)]
    pub extra: Map<String, Value>,
}

/// The uncompressed body of `layer_images/layer_<n>.pwszImg`.
#[derive(Debug, Clone, PartialEq)]
pub struct PwszLayerImage {
    pub index: u32,
    pub bytes: Vec<u8>,
}

impl PwszLayerImage {
    /// Returns the fixed, observed 48-byte little-endian prefix. `tail` is
    /// intentionally opaque until the segment encoding has been fully proved.
    pub fn header(&self) -> Result<PwszImageHeader> {
        PwszImageHeader::parse(&self.bytes)
    }

    pub fn payload(&self) -> Result<&[u8]> {
        Ok(&self.bytes[PwszImageHeader::LEN..])
    }
}

/// Observed fixed prefix of native `pwszImg` data.
#[derive(Debug, Clone, PartialEq)]
pub struct PwszImageHeader {
    /// Observed sample bytes: `7b 3d 3d 00` (`"{==\\0"`). The writer code
    /// alone does not yet prove its semantic name, so it is not validated.
    pub magic: [u8; 4],
    /// Four little-endian IEEE-754 values at offsets `0x04..0x13`.
    pub bounds: [f32; 4],
    /// Bytes `0x14..0x2f`, kept verbatim because their field layout is not yet
    /// fully established.
    pub tail: [u8; 28],
}

impl PwszImageHeader {
    pub const LEN: usize = 48;

    fn parse(bytes: &[u8]) -> Result<Self> {
        if bytes.len() < Self::LEN {
            return Err(Error::Protocol(
                "pwszImg is shorter than its observed header",
            ));
        }
        let mut magic = [0; 4];
        magic.copy_from_slice(&bytes[..4]);
        let bounds = std::array::from_fn(|index| {
            let offset = 4 + index * 4;
            f32::from_le_bytes(bytes[offset..offset + 4].try_into().expect("fixed range"))
        });
        let mut tail = [0; 28];
        tail.copy_from_slice(&bytes[20..Self::LEN]);
        Ok(Self {
            magic,
            bounds,
            tail,
        })
    }
}

fn read_json<R: Read + Seek, T: for<'de> Deserialize<'de>>(
    archive: &mut ZipArchive<R>,
    name: &str,
) -> Result<T> {
    let mut file = archive.by_name(name)?;
    Ok(serde_json::from_reader(&mut file)?)
}

fn read_json_optional<R: Read + Seek>(
    archive: &mut ZipArchive<R>,
    name: &str,
) -> Result<Option<Value>> {
    match archive.by_name(name) {
        Ok(mut file) => Ok(Some(serde_json::from_reader(&mut file)?)),
        Err(zip::result::ZipError::FileNotFound) => Ok(None),
        Err(error) => Err(error.into()),
    }
}

fn parse_index(name: &str, prefix: &str, suffix: &str) -> Option<u32> {
    name.strip_prefix(prefix)?
        .strip_suffix(suffix)?
        .parse()
        .ok()
}

#[cfg(test)]
mod tests {
    use std::io::Cursor;

    use zip::{CompressionMethod, ZipWriter, write::SimpleFileOptions};

    use super::*;

    fn json_profile() -> &'static str {
        r#"{
            "machine_type": {
                "name":"Test", "key_suffix":"pwsz", "key_image_format":"pwszImg",
                "res_x":1, "res_y":1, "xy_pixel":1.0, "xy_pixel_y":1.0,
                "print_xsize":1.0, "print_ysize":1.0, "print_zsize":1.0
            },
            "machine_extern":{"alias":"Test"}
        }"#
    }

    #[test]
    fn reads_known_pwsz_members_without_decoding_the_layer_stream() {
        let mut writer = ZipWriter::new(Cursor::new(Vec::new()));
        let options = SimpleFileOptions::default().compression_method(CompressionMethod::Deflated);
        for (name, contents) in [
            (MACHINE_PROFILE, json_profile().as_bytes()),
            (PRINT_INFO, br#"{"print_time": 42}"# as &[u8]),
            (LAYERS, br#"{"count":1,"paras":[{"layer_index":0,"layer_minheight":0.0,"layer_thickness":0.05,"exposure_time":1.0}]}"# as &[u8]),
            (SOFTWARE_INFO, br#"{"version":"test"}"# as &[u8]),
            ("preview_images/preview_0.png", b"png" as &[u8]),
        ] {
            writer.start_file(name, options).unwrap();
            std::io::Write::write_all(&mut writer, contents).unwrap();
        }
        let mut layer = [0_u8; PwszImageHeader::LEN];
        layer[..4].copy_from_slice(&[0x7b, 0x3d, 0x3d, 0]);
        layer[4..8].copy_from_slice(&1.0_f32.to_le_bytes());
        layer[8..12].copy_from_slice(&2.0_f32.to_le_bytes());
        layer[12..16].copy_from_slice(&3.0_f32.to_le_bytes());
        layer[16..20].copy_from_slice(&4.0_f32.to_le_bytes());
        writer
            .start_file("layer_images/layer_0.pwszImg", options)
            .unwrap();
        std::io::Write::write_all(&mut writer, &layer).unwrap();
        let cursor = writer.finish().unwrap();
        let mut job = PwszArchive::from_reader(Cursor::new(cursor.into_inner())).unwrap();

        assert_eq!(job.metadata().layer_indices, [0]);
        assert_eq!(job.metadata().preview_indices, [0]);
        assert_eq!(job.metadata().print_info.print_time, Some(42));
        assert_eq!(job.read_preview(0).unwrap(), b"png");

        let image = job.read_layer(0).unwrap();
        let header = image.header().unwrap();
        assert_eq!(header.magic, [0x7b, 0x3d, 0x3d, 0]);
        assert_eq!(header.bounds, [1.0, 2.0, 3.0, 4.0]);
        assert!(image.payload().unwrap().is_empty());
    }
}
