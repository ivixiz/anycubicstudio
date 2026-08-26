//! Diagnostic CLI for LAN discovery, PWSZ inspection and Anycubic Cloud.
//! `cloud-start`, `cloud-pause`, `cloud-resume` and `cloud-stop` are active
//! hardware commands; discovery, inspection and list commands are read-only.
//!
//! With no arguments it sends the exact SSDP discovery request. To fetch an
//! advertised endpoint, pass both its host and port, e.g.
//! `cargo run -- 192.168.0.101 18910`.  The port is intentionally not guessed.
//! To inspect a print job locally, use `cargo run -- pwsz path/to/file.pwsz`.

use std::time::Duration;

use anycubicstudio::cloud::CloudClient;
use anycubicstudio::discovery::{discover, fetch_location_info};
use anycubicstudio::pwsz::PwszArchive;
use anycubicstudio::slicer::{SlicerConfig, SupportConfig, SupportShape};

#[tokio::main]
async fn main() -> anyhow::Result<()> {
    let mut arguments = std::env::args().skip(1);
    let host = arguments.next();
    if host.as_deref() == Some("slice") {
        let (input, output, config) = parse_slice_arguments(arguments)?;
        let report = anycubicstudio::slicer::slice_file(input, output, &config)?;
        println!(
            "Created PWSZ: {} model + {} support triangles ({} supports), {} layers, {:.3} ml, {} bytes; bounds {:?} mm",
            report.model_triangle_count,
            report.support_triangle_count,
            report.support_count,
            report.layer_count,
            report.estimated_volume_ml,
            report.output_bytes,
            report.bounds_mm,
        );
        return Ok(());
    }
    if host.as_deref() == Some("viewer-scene") {
        let (input, output, config) = parse_slice_arguments(arguments)?;
        let report = anycubicstudio::slicer::export_viewer_scene(input, output, &config)?;
        println!(
            "Created viewer scene: {} model + {} support triangles ({} supports), {} layers, {} bytes",
            report.model_triangle_count,
            report.support_triangle_count,
            report.support_count,
            report.layer_count,
            report.output_bytes,
        );
        return Ok(());
    }
    if host.as_deref() == Some("viewer") {
        let input = arguments.next().ok_or_else(|| {
            anyhow::anyhow!("usage: anycubicstudio viewer <MODEL> [slice options]")
        })?;
        let parse_arguments = std::iter::once(input.clone())
            .chain(std::iter::once("unused.pwsz".to_owned()))
            .chain(arguments);
        let (_, _, config) = parse_slice_arguments(parse_arguments)?;
        let scene_path = std::env::temp_dir().join(format!(
            "anycubic-viewer-{}.acv",
            uuid::Uuid::new_v4().simple()
        ));
        let report = anycubicstudio::slicer::export_viewer_scene(&input, &scene_path, &config)?;
        println!(
            "Opening viewer: {} model + {} support triangles ({} supports), {} layers",
            report.model_triangle_count,
            report.support_triangle_count,
            report.support_count,
            report.layer_count,
        );
        let viewer = locate_viewer()?;
        let status = std::process::Command::new(&viewer)
            .arg(&scene_path)
            .status();
        let _ = std::fs::remove_file(&scene_path);
        let status = status.map_err(|error| {
            anyhow::anyhow!("could not start viewer {}: {error}", viewer.display())
        })?;
        if !status.success() {
            return Err(anyhow::anyhow!("viewer exited with {status}"));
        }
        return Ok(());
    }
    if host.as_deref() == Some("slicer-default-config") {
        let path = arguments.next().ok_or_else(|| {
            anyhow::anyhow!("usage: anycubicstudio slicer-default-config <output.json>")
        })?;
        if arguments.next().is_some() {
            return Err(anyhow::anyhow!(
                "usage: anycubicstudio slicer-default-config <output.json>"
            ));
        }
        std::fs::write(path, serde_json::to_vec_pretty(&SlicerConfig::default())?)?;
        return Ok(());
    }
    if matches!(
        host.as_deref(),
        Some("cloud-status") | Some("cloud-status-detail")
    ) {
        let detailed_status = host.as_deref() == Some("cloud-status-detail");
        let path = arguments.next().ok_or_else(|| {
            anyhow::anyhow!(
                "usage: anycubicstudio cloud-status <Workshop-global_config.ini> [cloud-api-root]"
            )
        })?;
        let cloud = match arguments.next() {
            Some(api_root) => CloudClient::with_api_root(
                anycubicstudio::cloud::WorkshopAccessToken::from_global_config(path)?,
                api_root,
            )?,
            None => CloudClient::from_global_config(path)?,
        };
        let printers = cloud.list_printers().await?;
        println!("Cloud returned {} printer(s).", printers.len());
        for (index, printer) in printers.iter().enumerate() {
            println!(
                "printer #{index}: id={:?}, name={:?}, model={:?}, firmware={:?}, machine={:?}, device_status={:?}, ready_status={:?}, is_printing={:?}, is_printring={:?}, progress={:?}, layers={:?}/{:?}, ip={:?}",
                printer.id,
                printer.name,
                printer.model,
                printer.firmware_version,
                printer.machine_version,
                printer.device_status,
                printer.ready_status,
                printer.is_printing,
                printer.is_printring,
                printer.progress_percent,
                printer.current_layer,
                printer.total_layers,
                printer.ip,
            );
        }
        if !detailed_status {
            return Ok(());
        }
        let statuses = cloud.list_printer_statuses().await?;
        println!(
            "Cloud status endpoint returned {} record(s).",
            statuses.len()
        );
        for (index, printer) in statuses.iter().enumerate() {
            println!(
                "status #{index}: id={:?}, name={:?}, model={:?}, firmware={:?}, machine={:?}, device_status={:?}, ready_status={:?}, is_printing={:?}, is_printring={:?}, progress={:?}, layers={:?}/{:?}, ip={:?}",
                printer.id,
                printer.name,
                printer.model,
                printer.firmware_version,
                printer.machine_version,
                printer.device_status,
                printer.ready_status,
                printer.is_printing,
                printer.is_printring,
                printer.progress_percent,
                printer.current_layer,
                printer.total_layers,
                printer.ip,
            );
        }
        return Ok(());
    }
    if host.as_deref() == Some("cloud-upload") {
        let config = arguments.next().ok_or_else(|| {
            anyhow::anyhow!(
                "usage: anycubicstudio cloud-upload <Workshop-global_config.ini> <printer-id> <file.pwsz> [cloud-api-root]"
            )
        })?;
        let printer_id = arguments
            .next()
            .ok_or_else(|| anyhow::anyhow!("cloud-upload requires a numeric printer id"))?
            .parse()?;
        let archive = arguments.next().ok_or_else(|| {
            anyhow::anyhow!(
                "usage: anycubicstudio cloud-upload <Workshop-global_config.ini> <printer-id> <file.pwsz> [cloud-api-root]"
            )
        })?;
        let cloud = match arguments.next() {
            Some(api_root) => CloudClient::with_api_root(
                anycubicstudio::cloud::WorkshopAccessToken::from_global_config(config)?,
                api_root,
            )?,
            None => CloudClient::from_global_config(config)?,
        };
        let transfer = cloud.upload_pwsz_to_printer(printer_id, archive).await?;
        println!(
            "Transferred {} byte(s) to Cloud file {} for printer {}. No print-start command was sent.",
            transfer.bytes_sent, transfer.cloud_file_id, transfer.printer_id
        );
        return Ok(());
    }
    if host.as_deref() == Some("cloud-inspect") {
        let config = arguments.next().ok_or_else(|| {
            anyhow::anyhow!(
                "usage: anycubicstudio cloud-inspect <Workshop-global_config.ini> [cloud-api-root]"
            )
        })?;
        let cloud = match arguments.next() {
            Some(api_root) => CloudClient::with_api_root(
                anycubicstudio::cloud::WorkshopAccessToken::from_global_config(config)?,
                api_root,
            )?,
            None => CloudClient::from_global_config(config)?,
        };
        let files = cloud.list_cloud_files(20).await?;
        println!("Cloud returned {} recent file(s).", files.len());
        for file in files {
            println!(
                "file id={}, name={:?}, bytes={:?}, status={:?}, gcode_id={:?}, extension={:?}, parse_status={:?}",
                file.id,
                file.name,
                file.size,
                file.status,
                file.gcode_id,
                file.extension,
                file.parse_status,
            );
        }
        let projects = cloud.list_projects(20).await?;
        println!("Cloud returned {} recent project(s).", projects.len());
        for project in projects {
            println!(
                "project id={}, task_id={:?}, printer_id={:?}, gcode_id={:?}, name={:?}, progress={:?}, print_status={:?}, status={:?}, create={:?}, start={:?}, end={:?}",
                project.id,
                project.task_id,
                project.printer_id,
                project.gcode_id,
                project.name,
                project.progress_percent,
                project.print_status,
                project.status,
                project.create_time,
                project.start_time,
                project.end_time,
            );
        }
        return Ok(());
    }
    if host.as_deref() == Some("cloud-projects") {
        let config = arguments.next().ok_or_else(|| {
            anyhow::anyhow!("usage: anycubicstudio cloud-projects <Workshop-global_config.ini>")
        })?;
        let cloud = CloudClient::from_global_config(config)?;
        let projects = cloud.list_projects(20).await?;
        println!("Cloud returned {} recent project(s).", projects.len());
        for project in projects {
            println!(
                "project id={}, task_id={:?}, printer_id={:?}, gcode_id={:?}, name={:?}, progress={:?}, print_status={:?}, status={:?}, create={:?}, start={:?}, end={:?}",
                project.id,
                project.task_id,
                project.printer_id,
                project.gcode_id,
                project.name,
                project.progress_percent,
                project.print_status,
                project.status,
                project.create_time,
                project.start_time,
                project.end_time,
            );
        }
        return Ok(());
    }
    if host.as_deref() == Some("cloud-start") {
        let config = arguments.next().ok_or_else(|| {
            anyhow::anyhow!(
                "usage: anycubicstudio cloud-start <Workshop-global_config.ini> <printer-id> <cloud-file-id>"
            )
        })?;
        let printer_id = arguments
            .next()
            .ok_or_else(|| anyhow::anyhow!("cloud-start requires a printer id"))?
            .parse()?;
        let cloud_file_id = arguments
            .next()
            .ok_or_else(|| anyhow::anyhow!("cloud-start requires a cloud file id"))?
            .parse()?;
        let cloud = CloudClient::from_global_config(config)?;
        let ack = cloud.start_cloud_print(printer_id, cloud_file_id).await?;
        println!(
            "Cloud accepted START_PRINT for file {cloud_file_id} on printer {printer_id}; msgid={}",
            ack.message_id
        );
        return Ok(());
    }
    if matches!(
        host.as_deref(),
        Some("cloud-pause") | Some("cloud-resume") | Some("cloud-stop") | Some("cloud-stop-active")
    ) {
        let operation = host.as_deref().expect("matched operation");
        let config = arguments.next().ok_or_else(|| {
            anyhow::anyhow!(
                "usage: anycubicstudio {operation} <Workshop-global_config.ini> <printer-id>"
            )
        })?;
        let printer_id = arguments
            .next()
            .ok_or_else(|| anyhow::anyhow!("{operation} requires a printer id"))?
            .parse()?;
        if arguments.next().is_some() {
            let explicit_operation = match operation {
                "cloud-pause" => "cloud-pause-project",
                "cloud-resume" => "cloud-resume-project",
                "cloud-stop" | "cloud-stop-active" => "cloud-stop-project",
                _ => unreachable!("operation was matched above"),
            };
            return Err(anyhow::anyhow!(
                "{operation} resolves the active project automatically; use {explicit_operation} <config> <printer-id> <project-id> for explicit control"
            ));
        }
        let cloud = CloudClient::from_global_config(config)?;
        let ack = match operation {
            "cloud-pause" => cloud.pause_active_cloud_print(printer_id).await?,
            "cloud-resume" => cloud.resume_active_cloud_print(printer_id).await?,
            "cloud-stop" | "cloud-stop-active" => cloud.stop_active_cloud_print(printer_id).await?,
            _ => unreachable!("operation was matched above"),
        };
        println!(
            "Cloud accepted {operation} for automatically resolved project {} on printer {printer_id}; msgid={}",
            ack.project_id, ack.message_id
        );
        return Ok(());
    }
    if matches!(
        host.as_deref(),
        Some("cloud-pause-project") | Some("cloud-resume-project") | Some("cloud-stop-project")
    ) {
        let operation = host.as_deref().expect("matched operation");
        let config = arguments.next().ok_or_else(|| {
            anyhow::anyhow!(
                "usage: anycubicstudio {operation} <Workshop-global_config.ini> <printer-id> <project-id>"
            )
        })?;
        let printer_id = arguments
            .next()
            .ok_or_else(|| anyhow::anyhow!("{operation} requires a printer id"))?
            .parse()?;
        let project_id = arguments
            .next()
            .ok_or_else(|| anyhow::anyhow!("{operation} requires a project id"))?
            .parse()?;
        let cloud = CloudClient::from_global_config(config)?;
        let ack = match operation {
            "cloud-pause-project" => cloud.pause_cloud_print(printer_id, project_id).await?,
            "cloud-resume-project" => cloud.resume_cloud_print(printer_id, project_id).await?,
            "cloud-stop-project" => cloud.stop_cloud_print(printer_id, project_id).await?,
            _ => unreachable!("operation was matched above"),
        };
        println!(
            "Cloud accepted {operation} for project {project_id} on printer {printer_id}; msgid={}",
            ack.message_id
        );
        return Ok(());
    }
    if host.as_deref() == Some("pwsz") {
        let path = arguments
            .next()
            .ok_or_else(|| anyhow::anyhow!("usage: anycubicstudio pwsz <file.pwsz>"))?;
        let mut job = PwszArchive::open(path)?;
        let metadata = job.metadata();
        println!("machine: {}", metadata.machine.name);
        println!(
            "LCD: {}x{}, build volume: {:.3} x {:.3} x {:.3} mm",
            metadata.machine.res_x,
            metadata.machine.res_y,
            metadata.machine.print_xsize,
            metadata.machine.print_ysize,
            metadata.machine.print_zsize,
        );
        println!(
            "job: {} layers, {} previews, {} ZIP entries, estimated {:?} s",
            metadata.layers.count,
            metadata.preview_indices.len(),
            metadata.entries.len(),
            metadata.print_info.print_time,
        );
        if let Some(index) = metadata.layer_indices.first().copied() {
            let image = job.read_layer(index)?;
            let header = image.header()?;
            println!(
                "first pwszImg: layer {index}, {} bytes, magic {:02x?}, bounds {:?}",
                image.bytes.len(),
                header.magic,
                header.bounds
            );
        }
        return Ok(());
    }
    let client = reqwest::Client::builder()
        .timeout(Duration::from_secs(10))
        .build()?;
    if let Some(host) = host {
        let port = arguments
            .next()
            .ok_or_else(|| anyhow::anyhow!("usage: anycubicstudio <host> <info-port>"))?
            .parse()?;
        let info = fetch_location_info(&client, &host, port).await?;
        println!("{:#?}", info);
        return Ok(());
    }
    for advertisement in discover(Duration::from_secs(10)).await? {
        println!("{} {:?}", advertisement.sender, advertisement.headers);
    }
    Ok(())
}

fn parse_slice_arguments(
    mut arguments: impl Iterator<Item = String>,
) -> anyhow::Result<(String, String, SlicerConfig)> {
    let input = arguments
        .next()
        .ok_or_else(|| anyhow::anyhow!(slice_usage()))?;
    let output = arguments
        .next()
        .ok_or_else(|| anyhow::anyhow!(slice_usage()))?;
    let mut config = SlicerConfig::default();
    let mut save_config = None;
    while let Some(option) = arguments.next() {
        let value = |arguments: &mut dyn Iterator<Item = String>| {
            arguments
                .next()
                .ok_or_else(|| anyhow::anyhow!("{option} requires a value"))
        };
        match option.as_str() {
            "--config" => {
                config = serde_json::from_slice(&std::fs::read(value(&mut arguments)?)?)?;
            }
            "--save-config" => {
                save_config = Some(value(&mut arguments)?);
            }
            "--supports" => {
                config.supports = match value(&mut arguments)?.as_str() {
                    "none" => SupportConfig::default(),
                    "light" => enabled_supports(SupportConfig::light()),
                    "medium" => enabled_supports(SupportConfig::medium()),
                    "heavy" => enabled_supports(SupportConfig::heavy()),
                    preset => {
                        return Err(anyhow::anyhow!(
                            "unknown support preset {preset}; expected none, light, medium or heavy"
                        ));
                    }
                };
            }
            "--support-shape" => {
                config.supports.shape = match value(&mut arguments)?.as_str() {
                    "cylinder" => SupportShape::Cylinder,
                    "tapered" => SupportShape::Tapered,
                    "square" => SupportShape::Square,
                    shape => {
                        return Err(anyhow::anyhow!(
                            "unknown support shape {shape}; expected cylinder, tapered or square"
                        ));
                    }
                };
            }
            "--support-auto" => {
                config.supports.auto_generate = match value(&mut arguments)?.as_str() {
                    "on" | "true" | "yes" | "1" => true,
                    "off" | "false" | "no" | "0" => false,
                    mode => {
                        return Err(anyhow::anyhow!(
                            "unknown automatic-support mode {mode}; expected on or off"
                        ));
                    }
                };
            }
            "--support-spacing" => config.supports.spacing_mm = value(&mut arguments)?.parse()?,
            "--support-angle" => {
                config.supports.overhang_angle_deg = value(&mut arguments)?.parse()?
            }
            "--support-tip" => config.supports.tip_diameter_mm = value(&mut arguments)?.parse()?,
            "--support-trunk" => {
                config.supports.trunk_diameter_mm = value(&mut arguments)?.parse()?
            }
            "--support-base" => {
                config.supports.base_diameter_mm = value(&mut arguments)?.parse()?
            }
            "--support-tip-height" => {
                config.supports.tip_height_mm = value(&mut arguments)?.parse()?
            }
            "--support-base-height" => {
                config.supports.base_height_mm = value(&mut arguments)?.parse()?
            }
            "--support-max-height" => {
                config.supports.max_height_mm = value(&mut arguments)?.parse()?
            }
            "--support-min-height" => {
                config.supports.min_height_mm = value(&mut arguments)?.parse()?
            }
            "--support-segments" => {
                config.supports.radial_segments = value(&mut arguments)?.parse()?
            }
            "--manual-support" => {
                let coordinates = parse_numbers(&value(&mut arguments)?, 3, 4)?;
                config.supports.enabled = true;
                config.supports.add_manual_point(
                    [coordinates[0], coordinates[1], coordinates[2]],
                    coordinates.get(3).copied(),
                );
            }
            "--layer-height" => config.print.layer_height_mm = value(&mut arguments)?.parse()?,
            "--bottom-layers" => config.print.bottom_layers = value(&mut arguments)?.parse()?,
            "--bottom-exposure" => {
                config.print.bottom_exposure_s = value(&mut arguments)?.parse()?
            }
            "--exposure" => config.print.exposure_s = value(&mut arguments)?.parse()?,
            "--lift-height" => config.print.lift_height_mm = value(&mut arguments)?.parse()?,
            "--lift-speed" => config.print.lift_speed_mm_s = value(&mut arguments)?.parse()?,
            "--move" => config.transform.translation_mm = parse_triplet(&value(&mut arguments)?)?,
            "--rotate" => config.transform.rotation_deg = parse_triplet(&value(&mut arguments)?)?,
            "--scale" => config.transform.scale = parse_triplet(&value(&mut arguments)?)?,
            "--no-center" => config.transform.auto_center_xy = false,
            "--no-place-on-bed" => config.transform.place_on_bed = false,
            "--help" | "-h" => return Err(anyhow::anyhow!(slice_usage())),
            unknown => return Err(anyhow::anyhow!("unknown slice option {unknown}")),
        }
    }
    if let Some(path) = save_config {
        std::fs::write(path, serde_json::to_vec_pretty(&config)?)?;
    }
    Ok((input, output, config))
}

fn enabled_supports(mut supports: SupportConfig) -> SupportConfig {
    supports.enabled = true;
    supports
}

fn parse_triplet(value: &str) -> anyhow::Result<[f64; 3]> {
    let values = parse_numbers(value, 3, 3)?;
    Ok([values[0], values[1], values[2]])
}

fn parse_numbers(value: &str, minimum: usize, maximum: usize) -> anyhow::Result<Vec<f64>> {
    let values = value
        .split(',')
        .map(str::trim)
        .map(str::parse)
        .collect::<std::result::Result<Vec<_>, _>>()?;
    if !(minimum..=maximum).contains(&values.len()) {
        return Err(anyhow::anyhow!(
            "expected {minimum}{} comma-separated numbers, got {value}",
            if minimum == maximum {
                String::new()
            } else {
                format!(" to {maximum}")
            }
        ));
    }
    Ok(values)
}

fn slice_usage() -> &'static str {
    "usage: anycubicstudio slice <MODEL> <OUTPUT.pwsz> [--config FILE] [--save-config FILE] [--supports none|light|medium|heavy] [--support-auto on|off] [--support-shape cylinder|tapered|square] [--support-spacing MM] [--support-angle DEG] [--support-tip MM] [--support-trunk MM] [--support-base MM] [--support-tip-height MM] [--support-base-height MM] [--support-min-height MM] [--support-max-height MM] [--support-segments N] [--manual-support X,Y,Z[,BASE_Z]] [--layer-height MM] [--bottom-layers N] [--bottom-exposure SEC] [--exposure SEC] [--lift-height MM] [--lift-speed MM_S] [--move X,Y,Z] [--rotate X,Y,Z] [--scale X,Y,Z] [--no-center] [--no-place-on-bed]"
}

fn locate_viewer() -> anyhow::Result<std::path::PathBuf> {
    if let Some(path) = std::env::var_os("ANYCUBIC_VIEWER") {
        return Ok(path.into());
    }
    let current = std::env::current_exe()?;
    let sibling = current
        .parent()
        .map(|directory| directory.join("anycubic-viewer"));
    for candidate in [
        sibling,
        Some(std::path::PathBuf::from("target/release/anycubic-viewer")),
        Some(std::path::PathBuf::from("viewer/build/anycubic-viewer")),
    ]
    .into_iter()
    .flatten()
    {
        if candidate.is_file() {
            return Ok(candidate);
        }
    }
    Err(anyhow::anyhow!(
        "anycubic-viewer was not found; build it with: cmake -S viewer -B viewer/build && cmake --build viewer/build"
    ))
}
