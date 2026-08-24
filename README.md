# anycubicstudio

Unofficial async Rust library and diagnostic CLI for interacting with Anycubic resin 3D printers. The protocol was reverse-engineered from Anycubic Photon Workshop 4.1.8 and tested on an Anycubic Photon Mono M7 Pro. 

The project supports two independent communication paths:

* **Anycubic Cloud:** printer and file listing, status retrieval, PWSZ upload, print start, pause, resume, and cancellation;
* **Local network:** SSDP discovery, HTTP bootstrap, and MQTT/TLS commands and reports.

> **Warning:** `cloud-start`, `cloud-pause`, `cloud-resume`, `cloud-stop`, and MQTT control commands operate real hardware. Before running them, make sure the file model matches the printer, the resin vat and build platform are installed correctly, and all mechanical movement is safe.

## Current Status

| Feature                              | Status                                                                                    |
| ------------------------------------ | ----------------------------------------------------------------------------------------- |
| Retrieve Cloud printer list          | Tested on M7 Pro                                                                          |
| Read Cloud files and projects        | Tested on M7 Pro                                                                          |
| Upload and Cloud parsing of `.pwsz`  | Tested on M7 Pro                                                                          |
| Start Cloud print, `order_id = 1`    | Tested on M7 Pro                                                                          |
| Cancel Cloud print, `order_id = 4`   | Tested on M7 Pro                                                                          |
| Pause/resume, `order_id = 2/3`       | Confirmed from Workshop code and an external implementation; not sent to the test printer |
| Automatic active-project resolution  | Fail-closed: requires exactly one known active project                                     |
| LAN SSDP/bootstrap/MQTT              | Implemented based on Workshop 4.1.8; test M7 Pro did not advertise the LAN API            |
| Standalone browser-based OAuth login | Not yet implemented; currently uses the token from an installed Workshop instance         |

During a live test, `example.pwsz` was uploaded, parsed by the Cloud, started, and cancelled after 64 seconds. The project transitioned to `print_status = 3`, progress remained at zero, and the printer subsequently reported the idle state again.

## Requirements

* Rust with Edition 2024 support;
* synchronized system date and time;
* for Cloud mode — at least one successful login through an installed Photon Workshop instance;
* for LAN mode — firmware that advertises `ac:3dprinter:lcd` via SSDP.

Project verification:

```bash
cargo test
cargo clippy --all-targets -- -D warnings
```

## Cloud Authentication

The current version does not accept a username or password. It reads the OAuth `accessToken` from Photon Workshop's existing `global_config.ini` and exchanges it for a short-lived `XX-Token`:

```text
POST /v3/public/loginWithAccessToken
```

Typical file location on Windows:

```text
%LOCALAPPDATA%\ANYCUBIC\AnycubicPhotonWorkshop_V4.1.8\global_config.ini
```

When running from Linux with a mounted Windows partition, provide the actual path to this file. Do not copy `global_config.ini` into the repository or expose its contents in logs.

The embedded `WORKSHOP_APP_SECRET` is a shared Photon Workshop application key, not a user secret. It is identical across accounts for this SDK version and is not derived from the user's password. The actual secrets are the `accessToken`, `XX-Token`, refresh token, and pre-signed upload URLs.

## Quick Start via CLI

Run all commands from the project root. In the examples below, replace values enclosed in angle brackets; do not type the brackets themselves.

The positional order is now uniform wherever the same values occur:

```text
<CONFIG> <PRINTER_ID> <OPERATION-SPECIFIC VALUE>
```

The public Rust methods follow the same rule: `printer_id` comes before a file,
Cloud file id, or project id.

### 1. Find the printer's Cloud ID

```bash
cargo run -- cloud-status <PATH_TO_GLOBAL_CONFIG.INI>
```

Detailed Cloud status:

```bash
cargo run -- cloud-status-detail <PATH_TO_GLOBAL_CONFIG.INI>
```

### 2. Validate a PWSZ file locally

This command does not send anything over the network:

```bash
cargo run -- pwsz <FILE.PWSZ>
```

Check the printer model, LCD resolution, build-area dimensions, and layer count.

The file extension alone does not guarantee compatibility; the application must perform this validation before issuing a start command.

### 3. Upload a file

```bash
cargo run -- cloud-upload \
  <PATH_TO_GLOBAL_CONFIG.INI> \
  <PRINTER_ID> \
  <FILE.PWSZ>
```

The command reserves storage space, performs the file `PUT`, registers the Cloud file, and calls `sendFile`. It **does not start printing**. Save the printed `cloud_file_id`.

### 4. Wait for the file to be parsed

```bash
cargo run -- cloud-inspect <PATH_TO_GLOBAL_CONFIG.INI>
```

Before starting, the target file should have a non-zero `gcode_id` and a completed `parse_status`. Do not poll the Cloud more often than once every few seconds because the server applies rate limiting.

### 5. Start printing

```bash
cargo run -- cloud-start \
  <PATH_TO_GLOBAL_CONFIG.INI> \
  <PRINTER_ID> \
  <CLOUD_FILE_ID>
```

A successful response contains a `msgid`, but this only indicates that the Cloud accepted the command. To confirm that printing actually started, locate the newly created project.

### 6. Monitor the project state

```bash
cargo run -- cloud-projects <PATH_TO_GLOBAL_CONFIG.INI>
```

Find the new project using its `printer_id`, `gcode_id`, and filename. The safe
pause/resume/stop commands resolve this project automatically, so normal CLI
usage does not require copying its `id`. The field is still useful for
monitoring and advanced explicit control. Never substitute `cloud_file_id` or
`gcode_id` for a `project_id`.

### 7. Pause, resume, and cancel

```bash
cargo run -- cloud-pause \
  <PATH_TO_GLOBAL_CONFIG.INI> <PRINTER_ID>

cargo run -- cloud-resume \
  <PATH_TO_GLOBAL_CONFIG.INI> <PRINTER_ID>

cargo run -- cloud-stop \
  <PATH_TO_GLOBAL_CONFIG.INI> <PRINTER_ID>
```

Each command first lists recent projects for the printer. It sends the order
only when exactly one project has a known active status and no completion
timestamp. It refuses to act when there are no active projects, multiple
candidates, or an unknown non-terminal status.

For advanced use, an exact project can still be controlled explicitly:

```bash
cargo run -- cloud-pause-project  <CONFIG> <PRINTER_ID> <PROJECT_ID>
cargo run -- cloud-resume-project <CONFIG> <PRINTER_ID> <PROJECT_ID>
cargo run -- cloud-stop-project   <CONFIG> <PRINTER_ID> <PROJECT_ID>
```

After sending a control command, run `cloud-projects` again and monitor the state transition. A `msgid` alone does not guarantee that the operation has completed.

## CLI Command Reference

| Command                                           | Purpose                                         | Effect                 |
| ------------------------------------------------- | ----------------------------------------------- | ---------------------- |
| `cargo run --`                                    | Search for LAN printers via SSDP for 10 seconds | Read-only              |
| `cargo run -- <HOST> <INFO_PORT>`                 | `GET http://HOST:PORT/info`                     | Read-only              |
| `cargo run -- pwsz <FILE>`                        | Validate PWSZ metadata                          | Local, read-only       |
| `cloud-status <CONFIG>`                           | List Cloud printers                             | Read-only              |
| `cloud-status-detail <CONFIG>`                    | List printers and detailed status               | Read-only              |
| `cloud-inspect <CONFIG>`                          | Show recent Cloud files and projects            | Read-only              |
| `cloud-projects <CONFIG>`                         | Show recent projects and their states           | Read-only              |
| `cloud-upload <CONFIG> <PRINTER_ID> <FILE>`       | Upload and deliver a file without starting it   | Modifies Cloud storage |
| `cloud-start <CONFIG> <PRINTER_ID> <FILE_ID>`     | Start a file, `order_id=1`                      | Active command         |
| `cloud-pause <CONFIG> <PRINTER_ID>`               | Safely resolve and pause the active project     | Active command         |
| `cloud-resume <CONFIG> <PRINTER_ID>`              | Safely resolve and resume the active project    | Active command         |
| `cloud-stop <CONFIG> <PRINTER_ID>`                | Safely resolve and cancel the active project    | Active command         |
| `cloud-*-project <CONFIG> <PRINTER_ID> <PROJECT_ID>` | Explicit advanced project control            | Active command         |

For `cloud-status`, `cloud-status-detail`, `cloud-inspect`, and `cloud-upload`, you can optionally pass a different HTTPS API root as the final argument. The default is:

```text
https://cloud-universe.anycubic.com/p/p/workbench/api
```

## Cloud Project States

Numeric values should be preserved in logs in their original form, since Anycubic may extend the enum in future firmware versions.

| `print_status` | Observed/externally confirmed meaning |
| -------------: | ------------------------------------- |
|            `1` | printing                              |
|            `2` | complete                              |
|            `3` | cancelled                             |
|            `4` | downloading                           |
|            `5` | checking                              |
|            `6` | preheating                            |
|            `7` | slicing                               |
|            `9` | levelling                             |

On the tested M7 Pro, `device_status = 1` meant **online**, while `is_printing = 1` meant **free/idle**. These are Cloud status codes, not JSON booleans.

## Using It as a Rust Library

### Reading Cloud state

```rust
use anycubicstudio::cloud::CloudClient;

#[tokio::main]
async fn main() -> Result<(), Box<dyn std::error::Error>> {
    let cloud = CloudClient::from_global_config(
        r"C:\path\to\global_config.ini",
    )?;

    for printer in cloud.list_printers().await? {
        println!(
            "id={:?} model={:?} online={:?} print_state={:?}",
            printer.id,
            printer.model,
            printer.device_status,
            printer.is_printing,
        );
    }

    for project in cloud.list_projects(20).await? {
        println!(
            "project={} printer={:?} progress={:?} status={:?}",
            project.id,
            project.printer_id,
            project.progress_percent,
            project.print_status,
        );
    }

    Ok(())
}
```

### Uploading and controlling

```rust
use anycubicstudio::cloud::CloudClient;

#[tokio::main]
async fn main() -> Result<(), Box<dyn std::error::Error>> {
    let cloud = CloudClient::from_global_config(
        r"C:\path\to\global_config.ini",
    )?;

    let printer_id = 123_456;

    // Uploads and sends the file to the printer, but does not start printing.
    let transfer = cloud
        .upload_pwsz_to_printer(printer_id, "job.pwsz")
        .await?;

    println!("cloud_file_id={}", transfer.cloud_file_id);

    // The operations below actively control the hardware. They are left
    // commented out so that copying this example cannot start the printer.
    //
    // Before calling start, the application should validate the file model,
    // wait for Cloud parsing to complete, and obtain explicit user confirmation:
    //
    // let ack = cloud
    //     .start_cloud_print(printer_id, transfer.cloud_file_id)
    //     .await?;
    //
    // println!("start accepted: {}", ack.message_id);
    //
    // Safe high-level calls resolve exactly one active project themselves:
    // cloud.pause_active_cloud_print(printer_id).await?;
    // cloud.resume_active_cloud_print(printer_id).await?;
    // cloud.stop_active_cloud_print(printer_id).await?;
    //
    // Explicit project-id variants remain available for advanced clients:
    // let project_id = 123_456_789_u64;
    // cloud.stop_cloud_print(printer_id, project_id).await?;

    Ok(())
}
```

In a real application, do not execute pause/resume/stop consecutively. After each command, wait for the corresponding report or project state change, and implement a timeout followed by a state re-check.

## LAN API

Workshop sends the following SSDP request:

```text
UDP 239.255.255.250:1900
ST: ac:3dprinter:lcd
```

If the printer responds, the sequence is:

```text
discover()
  → LOCATION / GET /info
  → authenticate(location, did)
  → decrypted LanInfo
  → Printer::connect(LanInfo)
  → MQTT/TLS query/control/report
```

Minimal example after obtaining the selected network interface's `host`, `port`, and MAC address:

```rust
use std::time::Duration;

use anycubicstudio::{
    Printer,
    commands::Command,
    discovery::{authenticate, fetch_location_info},
    protocol::workshop_did_from_mac,
    status::PrintProgress,
};

#[tokio::main]
async fn main() -> Result<(), Box<dyn std::error::Error>> {
    let http = reqwest::Client::builder()
        .timeout(Duration::from_secs(10))
        .build()?;

    let location = fetch_location_info(&http, "192.168.1.100", 18910).await?;
    let did = workshop_did_from_mac([0x02, 0x00, 0x00, 0x00, 0x00, 0x01]);

    let lan_info = authenticate(&http, &location, &did).await?;

    let printer = Printer::connect(lan_info).await?;

    let msgid = printer.query(Command::print_query()).await?;

    let reply = printer
        .wait_for_msgid(&msgid, Duration::from_secs(10))
        .await?;

    println!("progress={:?}", PrintProgress::from_report(&reply));

    // LanInfo contains active MQTT credentials. Do not persist or print
    // the entire structure.

    printer.disconnect().await?;

    Ok(())
}
```

Port `18910` above is only an example. Use the port provided in the SSDP `LOCATION`; the library intentionally does not guess it. Workshop's fallback MQTT port is `8883`, but an explicit port from `LanInfo.broker` takes precedence.

### Confirmed LAN commands

| Constructor                              | MQTT `type`     | `action`    | Purpose                                           |
| ---------------------------------------- | --------------- | ----------- | ------------------------------------------------- |
| `Command::status_query()`                | `status`        | `query`     | General status                                    |
| `Command::lan_info_query()`              | `lanInfo`       | `query`     | LAN information                                   |
| `Command::print_query()`                 | `print`         | `query`     | Print status                                      |
| `Command::peripherie_query()`            | `peripherie`    | `query`     | Peripherals                                       |
| `Command::light_query()`                 | `light`         | `query`     | Light status                                      |
| `Command::auto_operation_status()`       | `autoOperation` | `getStatus` | Automatic functions/telemetry                     |
| `Command::signal_strength_query()`       | `properties`    | `read`      | Wi-Fi signal strength                             |
| `Command::connection_properties_query()` | `properties`    | `read`      | Connection property                               |
| `Command::pause()`                       | `print`         | `pause`     | Pause                                             |
| `Command::resume()`                      | `print`         | `resume`    | Resume                                            |
| `Command::stop()`                        | `print`         | `stop`      | Stop                                              |
| `Command::light(bool)`                   | `light`         | `control`   | Light control                                     |
| `Command::start_print(...)`              | `print`         | `start`     | Start a LAN print with explicitly supplied fields |

`Command::start_print` does not fill in unknown `task_mode`, `filetype`, or `project_type` values. The caller must obtain them from a verified source.

## Error Handling and Monitoring

All library methods return `anycubicstudio::Result<T>`. The main error variants are:

* `Http`, `Io`, `Mqtt`, `MqttConnection` — transport errors;
* `Timeout`, `Disconnected` — waiting for a report timed out or the connection was lost;
* `Cloud` — the server rejected a Cloud request;
* `BootstrapRejected` — the printer rejected LAN bootstrap;
* `Protocol` — a required field is missing or an operation is unsafe;
* `Json`, `Zip`, `Base64` — corrupted or malformed data.

Recommended Cloud monitoring loop:

1. Save the `msgid` of the command that was sent.
2. Poll `list_projects()` at a reasonable interval with a timeout.
3. Match the project using `printer_id`, `gcode_id`, and timestamp together.
4. Do not treat HTTP 200 as proof that the physical operation was performed.
5. After `stop`, wait for `print_status = 3` or a confirmed idle state.
6. After a network error, re-read the current state before blindly repeating an active command.

For LAN, use `Printer::reports()` and `Printer::wait_for_msgid()`. Older firmware versions may not return a `msgid`; in that case, a timeout means **"not confirmed"**, not necessarily **"not executed."**

## Project Structure

```text
src/
├── lib.rs        public modules
├── cloud.rs      Cloud auth, status, upload, and control
├── discovery.rs  SSDP, /info, and signed bootstrap
├── protocol.rs   structures, signatures, MQTT topics
├── transport.rs  MQTT/TLS transport and report stream
├── printer.rs    high-level LAN Printer
├── commands.rs   confirmed LAN commands
├── upload.rs     direct LAN HTTP upload
├── pwsz.rs       conservative PWSZ parser
├── status.rs     progress and telemetry parsing
└── error.rs      typed errors
```

Reverse-engineering details, function addresses, and JSON packets are documented in `PROTOCOL.md`. Slicer format and PWSZ analysis are documented in `SLICER.md`.

## Known Limitations

* The library does not yet perform the OAuth authorization-code flow on its own.
* The Cloud API is undocumented by the manufacturer and may change.
* Cloud pause/resume codes have not been live-tested on the current M7 Pro.
* The test M7 Pro did not respond to Workshop SSDP, so Cloud communication was used instead.
* `pwsz.rs` reads verified metadata and the layer container but is not a full slicer.
* The library does not schedule print jobs and does not replace the printer's hardware safety interlocks.

The project is not affiliated with Anycubic and uses an undocumented protocol. Before publishing or distributing it, check the applicable licenses and terms of service.
