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
| STL and Core mesh-3MF slicing        | Implemented natively in Rust; structurally validated PWSZ                                 |
| STEP/STP/IGES slicing                | Implemented through Gmsh tessellation; tested with `example.step`                         |
| Automatic/manual vertical supports   | Light/medium/heavy presets, three pillar shapes, CLI/JSON/Rust/C configuration             |
| PWSZ rendered previews               | Three real software-rendered views: 224×168, 336×252, and 800×600                          |
| 3D and sliced-layer viewer           | Qt 6/OpenGL viewer with model/support visibility, camera, zoom, and layer slider           |
| `libanycubicstudio.so` C ABI         | Builds and exports configuration, slicing, and explicit manual-support functions           |
| Generated PWSZ on physical printer   | Not tested yet; `calc_layer_volumes.data` adaptive grouping remains experimental            |

During a live test, `example.pwsz` was uploaded, parsed by the Cloud, started, and cancelled after 64 seconds. The project transitioned to `print_status = 3`, progress remained at zero, and the printer subsequently reported the idle state again.

## Requirements

* Rust with Edition 2024 support;
* `gmsh` in `PATH` for STEP/STP/IGES input (not needed for STL or Core mesh-3MF);
* CMake 3.21+, Qt 6 Widgets/OpenGL/OpenGLWidgets, and a working OpenGL 3.3 driver for the optional viewer;
* synchronized system date and time;
* for Cloud mode — at least one successful login through an installed Photon Workshop instance;
* for LAN mode — firmware that advertises `ac:3dprinter:lcd` via SSDP.

Project verification:

```bash
cargo test
cargo clippy --all-targets -- -D warnings
```

## Standalone PWSZ Slicer

The default profile targets **Anycubic Photon Mono M7 Pro** and uses the resin settings recovered from the supplied Workshop job: 0.05 mm layers, five 28 s bottom layers, 1.8 s normal exposure, and 8 mm lift at 6 mm/s.

```bash
cargo run --release -- slice \
  ../anycubic_disassembled/example.step \
  example.pwsz

cargo run -- pwsz example.pwsz
```

The slicer accepts STL, the Core mesh/build/component subset of 3MF, STEP/STP, and IGES. By default it centres the model in XY and places its lowest Z point on the bed. It rejects models outside the M7 Pro build volume.

Each generated PWSZ contains three rendered model previews, per-layer vector
polygons, resin/machine settings, print estimates, `scene.slice`, and
`calc_layer_volumes.data`. The generated file is structurally compatible with
the recovered PWSZ contract, but has not yet been accepted by the physical
printer. Inspect it locally and do not start a print until the remaining
firmware-specific member has been validated.

### Recommended local workflow

Build the slicer and viewer once:

```bash
cargo build --release
cmake -S viewer -B viewer/build -DCMAKE_BUILD_TYPE=Release
cmake --build viewer/build --parallel
```

Inspect a transformed model and generated supports before writing PWSZ:

```bash
target/release/anycubicstudio viewer model.stl --supports medium
```

The first viewer mode renders the triangle mesh in teal and supports in blue.
Drag with the left mouse button to rotate and use the wheel to zoom. Switch to
`Sliced layer` and move the slider to inspect the exact final contours that will
be encoded into `layer_<n>.pwszImg`. Model and supports are sliced separately
and their oriented contours are merged, preventing a support inside a model
outline from being misclassified as a hole.

After inspecting the orientation and supports, create and validate the job:

```bash
target/release/anycubicstudio slice model.stl model.pwsz --supports medium
target/release/anycubicstudio pwsz model.pwsz
unzip -t model.pwsz
```

`viewer-scene` is the non-interactive equivalent. It writes an `.acv` scene
that can be opened later or by another process:

```bash
target/release/anycubicstudio viewer-scene \
  model.stl model.acv --supports medium
target/release/anycubic-viewer model.acv
```

The `.acv` file is an internal inspection format, not a printer format and not
an editable Workshop `.pwscene` project.

### Slicer CLI options

The same options are accepted by `slice`, `viewer`, and `viewer-scene`. For
`viewer`, omit the output path.

| Option | Meaning |
| --- | --- |
| `--config FILE` | Load the complete `SlicerConfig` JSON before applying subsequent CLI overrides. |
| `--save-config FILE` | Save the final configuration after every option has been applied. |
| `--supports none\|light\|medium\|heavy` | Disable supports or select a complete automatic-support preset. |
| `--support-auto on\|off` | Enable/disable automatic contacts while retaining manual contacts and dimensions. |
| `--support-shape cylinder\|tapered\|square` | Select the generated pillar cross-section/profile. |
| `--support-spacing MM` | XY sampling/grid spacing for automatic contacts. Smaller values create more supports. |
| `--support-angle DEG` | Downward overhang threshold, strictly between 1° and 89°. |
| `--support-tip MM` | Contact-tip diameter. |
| `--support-trunk MM` | Main shaft diameter. Must be at least the tip diameter. |
| `--support-base MM` | Build-plate foot diameter. Must be at least the trunk diameter. |
| `--support-tip-height MM` | Length of the tapered contact section. |
| `--support-base-height MM` | Height of the build-plate foot. |
| `--support-min-height MM` | Ignore shorter automatic supports; useful for suppressing contacts on a model already flat on the bed. |
| `--support-max-height MM` | Reject supports longer than this value. |
| `--support-segments N` | Radial polygon resolution for cylinder/tapered supports; square always uses four sides. |
| `--manual-support X,Y,Z[,BASE_Z]` | Add one support contact. If `BASE_Z` is omitted, the nearest surface below or the plate is used. Repeatable. |
| `--layer-height MM` | Layer thickness. Default: 0.05 mm. |
| `--bottom-layers N` | Number of bottom layers. Default: 5. |
| `--bottom-exposure SEC` | Bottom-layer exposure. Default: 28 s. |
| `--exposure SEC` | Normal-layer exposure. Default: 1.8 s. |
| `--lift-height MM` | Lift distance. Default: 8 mm. |
| `--lift-speed MM_S` | Lift/retract speed used in the generated profile. Default: 6 mm/s. |
| `--move X,Y,Z` | Translation in millimetres, applied after centring/bed placement. |
| `--rotate X,Y,Z` | Euler rotations in degrees. |
| `--scale X,Y,Z` | Per-axis positive scale. |
| `--no-center` | Preserve the imported XY position instead of centring the transformed bounds. |
| `--no-place-on-bed` | Preserve imported Z position instead of moving the lowest point to Z=0. |

Support preset values:

| Preset | Spacing | Tip | Trunk | Base | Tip height | Other defaults |
| --- | ---: | ---: | ---: | ---: | ---: | --- |
| light | 5.0 mm | 0.35 mm | 0.8 mm | 3.5 mm | 1.5 mm | tapered, 45°, base height 0.8 mm |
| medium | 4.0 mm | 0.5 mm | 1.2 mm | 4.5 mm | 1.5 mm | tapered, 45°, base height 0.8 mm |
| heavy | 3.0 mm | 0.8 mm | 1.8 mm | 6.0 mm | 2.0 mm | tapered, 45°, base height 0.8 mm |

All three use 0.15 mm contact penetration, a 2 mm minimum height, a 180 mm
maximum height, and 12 radial segments. A later CLI option overrides the value
selected by the preset. `--supports none` resets the support section to
disabled medium defaults and clears manual contacts already loaded on the CLI;
place any later `--manual-support` after it.

Examples:

```bash
# Rotate, move, and use light tapered supports.
target/release/anycubicstudio viewer model.stl \
  --rotate 20,0,90 --move 5,0,8 --supports light

# Manual-only supports. Repeat --manual-support as needed.
target/release/anycubicstudio slice model.stl model.pwsz \
  --supports medium --support-auto off \
  --manual-support 12.5,-4.0,31.2 \
  --manual-support '-8.0,2.0,24.0,0.0'

# Save a reproducible JSON profile after applying overrides.
target/release/anycubicstudio slice model.stl model.pwsz \
  --supports heavy --support-tip 0.7 --save-config model-config.json
```

Quote a `--manual-support` value when its first coordinate is negative so the
shell passes it as one argument.

Generate a complete default JSON file without slicing:

```bash
target/release/anycubicstudio slicer-default-config slicer.json
```

The support portion of that JSON has the following shape and may be edited
directly. Enum strings use `snake_case`:

```json
{
  "supports": {
    "enabled": true,
    "auto_generate": false,
    "shape": "tapered",
    "overhang_angle_deg": 45.0,
    "spacing_mm": 4.0,
    "tip_diameter_mm": 0.5,
    "trunk_diameter_mm": 1.2,
    "base_diameter_mm": 4.5,
    "tip_height_mm": 1.5,
    "base_height_mm": 0.8,
    "penetration_mm": 0.15,
    "min_height_mm": 2.0,
    "max_height_mm": 180.0,
    "radial_segments": 12,
    "manual_points": [
      {"contact_mm": [4.0, -3.0, 18.0], "base_z_mm": null}
    ]
  }
}
```

This is only a fragment for explanation; `--config` expects the complete
`SlicerConfig` produced by `slicer-default-config` or `--save-config`.

The preset algorithm generates independent vertical tiered pillars. It is not
yet Workshop's branched/tree-support engine. Always inspect contacts and sliced
layers, especially for islands, internal cavities, fragile details, and meshes
with inconsistent triangle winding.

If `viewer` cannot find its executable, set `ANYCUBIC_VIEWER` to an absolute
path. The default search checks the slicer binary's directory,
`target/release/anycubic-viewer`, and `viewer/build/anycubic-viewer`. The viewer
requires a graphical session and OpenGL 3.3; it is not intended for headless
servers. PWSZ generation itself is headless and does not depend on Qt/OpenGL.

### Rust API

```rust
use anycubicstudio::slicer::{SlicerConfig, SupportShape, slice_file};

fn main() -> anycubicstudio::Result<()> {
    let mut config = SlicerConfig::default();
    config.transform.rotation_deg = [0.0, 0.0, 90.0];
    config.transform.translation_mm = [10.0, 0.0, 0.0];
    config.print.layer_height_mm = 0.05;
    config.supports.enabled = true;
    config.supports.auto_generate = true;
    config.supports.shape = SupportShape::Tapered;
    config.supports.tip_diameter_mm = 0.5;
    config.supports.trunk_diameter_mm = 1.2;
    config.supports.add_manual_point([4.0, -3.0, 18.0], None);

    let report = slice_file("model.step", "model.pwsz", &config)?;
    println!("{} layers, {:.3} ml", report.layer_count, report.estimated_volume_ml);
    Ok(())
}
```

Scale, move, rotate, exposure, lift, machine/profile values, automatic support
presets, and manual support contacts are represented in the public Rust API.
Hollowing and non-zero XY contour offset still return
`UnsupportedSlicerOption`; they are not silently ignored.

### Shared library / Bambu Studio integration

```bash
cargo build --release
```

This produces `target/release/libanycubicstudio.so`. The stable C interface is declared in [`include/anycubic_slicer.h`](include/anycubic_slicer.h):

```cpp
#include "anycubic_slicer.h"
#include <cstdio>

int main() {
    AnycubicSlicerConfig config;
    if (anycubic_slicer_default_config(&config) != 0)
        return 1;

    config.rotation_deg[2] = 90.0;
    config.supports_enabled = 1;
    config.supports_auto_generate = 1;
    config.support_shape = 1; /* tapered */
    if (anycubic_slicer_slice("model.step", "model.pwsz", &config) != 0) {
        std::fprintf(stderr, "%s\n", anycubic_slicer_last_error());
        return 1;
    }
}
```

For explicit contacts call `anycubic_slicer_slice_with_supports`. A `NAN`
`base_z_mm` asks the generator to find the nearest model surface below the
contact and then fall back to the build plate:

```cpp
#include "anycubic_slicer.h"
#include <cmath>

AnycubicSupportPoint points[] = {
    {{4.0, -3.0, 18.0}, NAN},
    {{12.0, 5.0, 24.0}, 0.0},
};

config.supports_enabled = 1;
config.supports_auto_generate = 0;
int result = anycubic_slicer_slice_with_supports(
    "model.stl", "model.pwsz", &config,
    points, sizeof(points) / sizeof(points[0]));
```

`support_shape` is `0` for cylinder, `1` for tapered, and `2` for square.
The returned error code is zero on success. On failure, read the thread-local
message immediately with `anycubic_slicer_last_error()`.

Example link flags:

```bash
c++ example.cpp -Iinclude -Ltarget/release \
  -Wl,-rpath,"$PWD/target/release" -lanycubicstudio
```

For Bambu Studio, add the header and shared library to a small CMake target and call the C ABI from the plate export action. STEP conversion currently starts `gmsh`; a later integration can pass Bambu Studio's already tessellated mesh through an additional in-memory ABI and avoid that process.

### Prepared test jobs

The requested local fixtures were generated with the M7 Pro profile and then
validated by both the strict project PWSZ parser and `unzip -t`:

| Output | Support decision | Result |
| --- | --- | --- |
| `test/core_last.pwsz` | `--supports medium`; the part is elevated and presents a large unsupported underside in the supplied orientation | 4,308 model triangles, 150 supports/18,000 support triangles, 1,320 layers, 42.243 ml estimate, 34,522,625 bytes |
| `test/trumna_last.pwsz` | `--supports none`; the part already lies on a broad flat face, and short edge supports were visually judged unnecessary | 880 model triangles, 0 supports, 320 layers, 11.748 ml estimate, 980,434 bytes |

SHA-256 checksums for the current files:

```text
aa63a22cd8173f4d9dd5f787303d844ead0a500ded3f16ba8e25931b2b44f179  test/core_last.pwsz
ccbb04a68b75774a34d1e27937094d96543fed702ff52eeffbad4f429f6c8c8e  test/trumna_last.pwsz
```

These jobs were not uploaded or started on the physical printer. In
particular, inspect all 150 contacts on `core_last` in the viewer before using
resin. Passing ZIP/binary validation proves container consistency, not that the
independent support placement is mechanically sufficient or that M7 Pro
firmware accepts the still-approximate volume grouping.

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
| `cargo run --release -- slice <MODEL> <PWSZ>`     | Slice STL/3MF/STEP/IGES to a local PWSZ         | Local file creation    |
| `slicer-default-config <JSON>`                    | Write the complete default slicer configuration | Local file creation    |
| `viewer <MODEL> [SLICE_OPTIONS]`                  | Generate a temporary scene and inspect it        | Local GUI/read-only    |
| `viewer-scene <MODEL> <ACV> [SLICE_OPTIONS]`      | Export the internal viewer scene without a GUI   | Local file creation    |
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
├── pwsz.rs       PWSZ metadata and vector-layer parser
├── slicer/
│   ├── mesh.rs          STL/3MF readers, CAD conversion, transforms
│   ├── geometry.rs      plane intersections, contours, layer merging
│   ├── support.rs       automatic/manual vertical supports
│   ├── render.rs        software-rendered PWSZ previews
│   ├── viewer_scene.rs  compact viewer interchange file
│   ├── writer.rs        PWSZ metadata and binary members
│   └── ffi.rs           stable C ABI used by libanycubicstudio.so
├── status.rs     progress and telemetry parsing
└── error.rs      typed errors

viewer/
├── CMakeLists.txt
└── main.cpp      Qt 6/OpenGL model, support, and sliced-layer viewer

include/
└── anycubic_slicer.h    public C/C++ header
```

Reverse-engineering details, function addresses, and JSON packets are documented in `PROTOCOL.md`. Slicer format and PWSZ analysis are documented in `SLICER.md`.

## Known Limitations

* The library does not yet perform the OAuth authorization-code flow on its own.
* The Cloud API is undocumented by the manufacturer and may change.
* Cloud pause/resume codes have not been live-tested on the current M7 Pro.
* The test M7 Pro did not respond to Workshop SSDP, so Cloud communication was used instead.
* Generated PWSZ files have passed structural validation but not a physical printer test.
* `calc_layer_volumes.data` has a confirmed container/record layout, but its adaptive M7 grouping algorithm is approximated with four-layer groups.
* Preview members now contain actual model/support renders at the expected 224×168, 336×252, and 800×600 dimensions. Their exact Workshop camera, lighting, and PNG compression are not byte-identical.
* Supports are independent vertical cylinder/tapered/square pillars. Workshop-compatible tree/branched supports, island-aware optimisation, cross-bracing, and exact VoxSDK preset parity are not implemented.
* Hollowing, drain holes, mesh repair, and XY contour compensation are not implemented yet.
* The built-in 3MF reader handles Core meshes, build items, components, units, and transforms. It rejects files that declare required extensions; optional material and texture data are not used for monochrome resin slicing.
* The library does not schedule print jobs and does not replace the printer's hardware safety interlocks.

The project is not affiliated with Anycubic and uses an undocumented protocol. Before publishing or distributing it, check the applicable licenses and terms of service.
