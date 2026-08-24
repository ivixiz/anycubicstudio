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

#[tokio::main]
async fn main() -> anyhow::Result<()> {
    let mut arguments = std::env::args().skip(1);
    let host = arguments.next();
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
