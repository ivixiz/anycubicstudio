# anycubicstudio

Unofficial async Rust library and diagnostic CLI for interacting with Anycubic resin 3D printers. The protocol was reverse-engineered from Anycubic Photon Workshop 4.1.8 and tested on an Anycubic Photon Mono M7 Pro. 

The project supports two independent communication paths:

* **Anycubic Cloud:** printer and file listing, status retrieval, PWSZ upload, print start, pause, resume, and cancellation;
* **Local network:** SSDP discovery, HTTP bootstrap, and MQTT/TLS commands and reports.

> **Warning:** `cloud-start`, `cloud-start-pcb`, `cloud-exposure-start`,
> `cloud-pause`, `cloud-resume`, `cloud-stop`, and MQTT control commands operate
> real hardware. A one-layer file with zero lift does **not** disable the
> firmware's initial Z homing and non-editable checks. Remove the build
> platform before using `cloud-start-pcb` for photoresist work, and protect
> eyes and skin from UV.

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
| Standalone browser-based OAuth login | Implemented with Workshop's browser authorization-code flow; passwords are never accepted by this program |
| STL and Core mesh-3MF slicing        | Implemented natively in Rust; structurally validated PWSZ                                 |
| STEP/STP/IGES slicing                | Implemented through Gmsh tessellation; tested with `example.step`                         |
| Automatic/manual vertical supports   | Light/medium/heavy presets, three pillar shapes, CLI/JSON/Rust/C configuration             |
| PWSZ rendered previews               | Three real software-rendered views: 224×168, 336×252, and 800×600                          |
| Graphical plate editor               | Qt 6/OpenGL multi-model GUI: drag/drop, selection, transform/mirror/layout, supports and M7 Pro build volume |
| GUI slicing and printer workflow     | 3D slice/upload/start and PCB slice/upload with a separate guarded start; status, pause/resume/stop through the Rust CLI |
| Gerber + Excellon PCB exposure       | Native M7-resolution composition, exact LCD preview and Cloud-parseable one-layer PWSZ; a normal start was observed to move Z |
| Built-in LCD service exposure        | Order 301/302 and firmware patterns 1/2/4 implemented; cannot display a caller-provided Gerber bitmap |
| `libanycubicstudio.so` C ABI         | Builds and exports configuration, slicing, and explicit manual-support functions           |
| Generated PWSZ compatibility         | Generated PCB PWSZ uploaded and parsed by Anycubic Cloud; normal start reached the printer and was cancelled after observed Z motion |

During a live test, the original `example.pwsz` was uploaded, parsed by the
Cloud, started, and cancelled after 64 seconds. A newly generated PCB PWSZ was
also accepted by the Cloud. Starting that one-layer file through the normal
print path caused platform/Z preparation despite zero configured lift, so it
was cancelled. This is why the GUI no longer starts PCB jobs immediately after
upload.

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

## Graphical plate editor and printer control

Build both the Rust engine and the Qt 6 frontend, then start the complete GUI:

```bash
cargo build --release
cmake -S viewer -B viewer/build -DCMAKE_BUILD_TYPE=Release
cmake --build viewer/build --parallel
target/release/anycubicstudio gui
```

Models may also be supplied at startup:

```bash
target/release/anycubicstudio gui model.stl enclosure.step parts.3mf
```

`ANYCUBICSTUDIO_CLI=/absolute/path/to/anycubicstudio` can be set when the Qt
executable and Rust executable are installed in different directories. The GUI
never reimplements slicing or Cloud packets in C++: it invokes the Rust
executable directly with `QProcess`, without a command shell. The bottom
`Rust API output` dock shows the exact operation and its result.

Viewer preferences persist through Qt `QSettings`. Window layout, view mode,
printing zone, exposure/motion values, PCB composition values, the last
applied support defaults, account-config path, printer ID, and built-in LCD
test values are restored on the next launch. The exact settings-file path is
printed in `Rust API output`. Account tokens remain only in the selected
`global_config.ini`; QSettings stores its path, never its contents. Imported
models, PWSZ paths and Cloud file IDs are deliberately not restored, preventing
a stale job from being started after reopening the program.

When the saved account config exists, the viewer performs a read-only
`cloud-status-detail` connection at startup and retains the detected printer
ID. `Connect / detect printer` repeats it manually. If no config path has been
saved, a `global_config.ini` in the working/project directory is used as a
convenience fallback.

### Plate workflow

1. Press the single `Import…` action, pass files on the command line, or drag
   them into the window. It accepts STL, mesh-3MF, STEP/STP, IGES/IGS, Gerber
   and Excellon DRL. The GUI classifies the input and selects the 3D or PCB
   pipeline automatically. CAD formats use Gmsh exactly as the headless Rust
   API does.
2. Select a model in `Models on plate`. The selected model is orange; other
   models are teal and supports are blue.
3. In `Transform`, configure Move, Rotate, positive scale magnitude, and the
   independent X/Y/Z Mirror switches, then press `Apply transform`. `Center
   X/Y`, `Place on bed`, and `Reset transform` are shortcuts. Mirror is encoded
   as a signed scale in the Rust project and triangle winding is corrected.
4. `Auto layout` places the centres of imported objects on non-overlapping
   shelf rows with a 5 mm margin. It refuses the operation if the current
   bounds do not fit the configured plate. It is a deterministic basic layout,
   not yet a polygon nesting optimiser.
5. Configure supports for the selected object on the `Supports` tab. None,
   Light, Medium, and Heavy presets fill the editable shape, overhang angle,
   spacing, contact, trunk, base, height, penetration, and radial-segment
   fields. `Automatic placement` can be disabled for manual
   `X,Y,Z[,BASE_Z]` contact points. Press `Apply supports and rebuild` to ask
   the Rust engine for a new preview.
6. In `Print & zone`, configure the build volume, layer/exposure and lift
   values. The default volume is the recovered Photon Mono M7 Pro profile:
   **223.642 × 126.480 × 230 mm**, 13312 × 5120 pixels. The bed grid and the
   complete three-dimensional volume outline are rendered in the 3D view.
7. Configure `Printer/Cloud`. For 3D input, `Slice & Run…` writes the PWSZ,
   uploads it and asks for final confirmation before starting. For Gerber/DRL,
   the same action is renamed `Slice & Upload…`; it writes and uploads the
   one-layer PWSZ but deliberately does not start it. After physically removing
   the platform, use the separate guarded PCB start button. After slicing,
   `Sliced layer` displays the exact contours.

3D models and PCB artwork cannot be combined in one print job: a resin model
has many height layers, while a PCB exposure deliberately has one stationary
layer. If both types are loaded, `Slice & Run…` fails closed and asks the user
to remove one type instead of silently choosing one.

Camera controls in 3D mode:

| Input | Action |
| --- | --- |
| left drag | orbit |
| Shift + left drag | pan |
| middle or right drag | pan |
| mouse wheel | zoom |
| `Reset camera` | reset orbit, zoom, and pan |

Switching between `Sliced layer` and `3D model & supports` explicitly restores
the OpenGL depth state. Front surfaces therefore remain opaque instead of
showing rear/internal faces after the `QPainter` layer view has been used.

Any geometry, support, printing-zone, or exposure change invalidates the
previous PWSZ and its Cloud file ID. `Slice & Run…`/`Slice & Upload…` always
rebuilds the job before upload, preventing a stale file from being printed by
mistake.

### GUI Cloud workflow

On `Printer/Cloud`, select Photon Workshop's `global_config.ini` and enter the
numeric Cloud printer ID returned by `cloud-status`. You may set
`ANYCUBIC_CONFIG` to prefill the configuration path. The file path is logged,
but its token contents are never displayed by the GUI.

The primary GUI action is deliberately unified:

| GUI action | Rust command | Effect |
| --- | --- | --- |
| `Slice & Run…` with 3D input | `project-slice` → `cloud-upload` → `cloud-start` | Rebuild, upload and start after confirmation |
| `Slice & Upload…` with Gerber/DRL | `pcb-slice` → `cloud-upload` | Create and deliver a one-layer job without moving the printer |
| `Start uploaded PCB (platform removed)…` | `cloud-start-pcb` | Start the last uploaded PCB with all confirmed editable preparation flags disabled |
| `Run built-in LCD test…` / `STOP LCD test` | `cloud-exposure-start` / `cloud-exposure-stop` | Motion-free firmware test pattern; not the imported Gerber image |
| `Printer status` | `cloud-status-detail <CONFIG>` | Read-only Cloud state |
| `Pause` / `Resume` | `cloud-pause` / `cloud-resume` | Resolves one active project safely, then controls it |
| `Stop` | `cloud-stop <CONFIG> <PRINTER_ID>` | Resolves and cancels one active project after confirmation |

The GUI extracts `CLOUD_FILE_ID` from the successful upload response, so no
file, G-code or project ID is copied manually. The upload happens before the
final start confirmation; cancelling that confirmation leaves an uploaded
Cloud file but does not send a start command. Pause/resume/stop use the
fail-closed active-project resolver described below. Closing a progress dialog
terminates the local Rust subprocess; after a network interruption, always
read printer status before repeating an active operation. For local-only PWSZ
creation without upload/start, use the explicit `slice`, `project-slice`, or
`pcb-slice` CLI command.

The GUI currently provides numeric transforms rather than an on-model gizmo,
and manual support contacts are entered as coordinates rather than picked with
a ray cast. Support geometry is the independent vertical-pillar engine
documented below, not Workshop/VoxSDK tree support parity.

## PCB photoresist exposure from KiCad Gerber and drill files

The Rust engine can compose one or more RS-274X Gerber files with one or more
Excellon drill files at the native **13312 × 5120** LCD resolution of the
Photon Mono M7 Pro. It then traces the final raster back into the vector
`pwszImg` representation and creates a normal PWSZ with exactly one layer. The
default layer/bottom exposure is **60 seconds** and the default configured lift
is zero.

This is intentionally implemented as a one-layer print job. It is **not** a
direct `UV on with this bitmap for N seconds` firmware command. A live test on
the M7 Pro showed that the normal print-start sequence moves/prepares Z even
when the PWSZ says zero lift. Layer thickness and lift settings only describe
layer motion; they do not suppress initial homing. The 60-second value is only
a starting example, not a calibrated recommendation for a particular resist,
film stack, transparency, LCD or UV intensity.

`cloud-start-pcb` uses a dedicated start profile recovered from Photon
Workshop's own request. It sets the following editable task options to zero:

```json
{
  "autoTop": 0,
  "dynamicRelease": 0,
  "failDetection": 0,
  "offLightCompensation": 0,
  "resinAutoLoad": 0,
  "resinHeat": 0,
  "stir_settings": {"stirTime": 0}
}
```

This disables automatic feed, preprint heating, stirring, end-of-print
auto-top, intelligent release, failure detection and off-light compensation
for that job. The printer reports `levelling`, `platform`, `resin`, and
`residual` as supported but `editable = 0`; omitting or falsifying those fields
cannot be claimed to disable them. The program therefore does not provide a
"no motion" promise for arbitrary Gerber PWSZ jobs.

### Coordinate composition and polarity

Gerber and DRL inputs from the same KiCad board are transformed as one group.
Their original board coordinates are preserved, so drill hits overlay their
pads directly; there is no independent centring or unreliable bounding-box
matching. The combined group is centred on the LCD by default. Rotation,
scale, mirror and XY offset are subsequently applied to the whole group.

The default composition rules are:

* dark Gerber primitives become UV-on LCD pixels;
* clear Gerber polarity removes pixels inside that Gerber layer;
* multiple Gerber files are OR-composited after each file's own dark/clear
  operations have been evaluated;
* `--drill-mode clear` removes Excellon hole/slot diameters, retaining visible
  alignment/drill centres in pads;
* `--invert` inverts only the transformed artwork rectangle, optionally
  enlarged with `--invert-margin`; it never turns the entire unused LCD on.

For negative dry-film resist, exposing the KiCad copper features is commonly
the useful starting polarity. Positive resist commonly needs the inverse, but
process chemistry and artwork orientation differ: always verify with a small
coupon. `--mirror-x`/`--mirror-y` are intended for contact-side orientation;
do not assume which one is correct without looking at text or an asymmetric
fiducial in the exact LCD preview.

Direct Rust CLI use enables X mirroring automatically when any Gerber filename
contains `F_Cu`, `F_Mask`, `F_Paste`, or `F_Silkscreen` (case-insensitive;
punctuation variants such as `F.Cu` are accepted). The whole Gerber+DRL group
is mirrored together, preserving drill alignment. Use
`--no-auto-mirror-front` or `--no-mirror-layers` to override this after
inspecting the preview. Back-side and unrelated filenames are not mirrored
automatically.

The parser currently covers the constructs used by ordinary KiCad plots:
metric/inch units, fixed coordinate formats, absolute/incremental coordinates,
circle/rectangle/obround/polygon apertures, common aperture macros, flashes,
linear and circular interpolation, regions, dark/clear polarity, Excellon
tools, drill hits, routes and `G85` slots. Exotic vendor aperture macros,
step-and-repeat and attributes that affect geometry still require a sample and
parser verification. Parser warnings are printed; they must not be ignored for
a production mask.

### PCB CLI workflow

Generate an exact viewer scene first, then create and inspect the one-layer
PWSZ:

```bash
target/release/anycubicstudio pcb-viewer-scene \
  board-F_Cu.gbr board.acv \
  --drill board-PTH.drl \
  --exposure 60 --drill-mode clear

target/release/anycubic-viewer board.acv

target/release/anycubicstudio pcb-slice \
  board-F_Cu.gbr board-photoresist.pwsz \
  --drill board-PTH.drl \
  --exposure 60 --drill-mode clear

target/release/anycubicstudio pwsz board-photoresist.pwsz
unzip -t board-photoresist.pwsz
```

Additional layers and drill files are repeatable:

```bash
target/release/anycubicstudio pcb-slice \
  board-F_Cu.gbr board.pwsz \
  --gerber board-F_Mask.gbr \
  --drill board-PTH.drl --drill board-NPTH.drl
```

The checked-in `test/pcb_demo-F_Cu.gbr` and
`test/pcb_demo-PTH.drl` form a small deterministic fixture. For example:

```bash
target/release/anycubicstudio pcb-slice \
  test/pcb_demo-F_Cu.gbr test/pcb_demo.pwsz \
  --drill test/pcb_demo-PTH.drl --exposure 60
```

The path was also exercised with the supplied KiCad 10.99
`DCDrillController-F_Cu.gbr`, `DCDrillController-PTH.drl`, and an empty but
valid `DCDrillController-NPTH.drl`. It produced one 60 s layer with 89
contours/19,252 vector segments and 2,172,412 UV-on pixels. The archive reader
confirmed 13312×5120, one `layer_0.pwszImg`, three previews, zero requested
lift and the expected 60 s exposure. This validates parsing and PWSZ structure,
not yet the physical photoresist exposure.

`pcb-default-config output.json` writes a complete editable
`PcbExposureConfig`. `--config` is applied when encountered and following
options override it; `--save-config` writes the final effective settings.

| PCB option | Meaning and default |
| --- | --- |
| `--gerber FILE` | Add another Gerber file; repeatable. The first Gerber is the required positional input. |
| `--drill FILE` | Add an Excellon drill/slot file; repeatable. |
| `--exposure SEC` | Exposure of the only layer; default 60 s. |
| `--drill-mode clear\|expose\|ignore` | Subtract, add, or parse without rendering drill geometry; default `clear`. |
| `--invert` / `--no-invert` | Invert/not invert the bounded artwork rectangle; default off. |
| `--invert-margin MM` | Expand the bounded inverse region; default 0. |
| `--mirror-x`, `--mirror-y` | Mirror the complete Gerber+DRL group. |
| `--auto-mirror-front`, `--no-auto-mirror-front` | Enable/disable filename-based front-layer X mirroring; enabled by default. |
| `--mirror-layers`, `--no-mirror-layers` | Explicitly enable/disable transfer-side X mirroring; disabling also suppresses the filename default. |
| `--rotate DEG` | Rotate the complete group around its source centre. |
| `--scale FACTOR` | Uniform scale; default 1.0. |
| `--move X,Y` | Offset from the LCD centre in millimetres. |
| `--simplify MM` | Pixel-boundary contour simplification tolerance; default 0.012 mm. Increase only if a complex job exceeds the printer's segment limit. |
| `--layer-height MM` | Synthetic single-layer thickness stored in the print profile; default 0.05 mm. |
| `--lift-height MM` | Requested post-layer lift; default 0 mm. This does not suppress firmware homing. |
| `--lift-speed MM_S` | Motion value stored in the generated profile; default 6 mm/s. |
| `--build-volume X,Y,Z` | Override the machine area; resolution remains the M7 Pro profile. |
| `--config FILE`, `--save-config FILE` | Load or save complete JSON configuration. |

### PCB workflow in the GUI

Use the same top `Import…` action as for STEP/STL/3MF, or drag all KiCad `.gbr`
and `.drl` files into the main window together. The PCB tab labels Gerber and
drill inputs separately and contains only PCB-specific parameters. Configure
exposure, drill mode, polarity, mirroring, rotation, scale and XY offset, then
press `Preview exact LCD layer`. In `Sliced layer`, cyan is UV-on and dark is
masked.

The `Mirror layers (X)` checkbox maps to Rust's `--mirror-layers` option.
`Auto from KiCad F_* filename` is enabled by default and recalculates that
checkbox whenever files are added or removed: `F_Cu`, `F_Mask`, `F_Paste`, or
`F_Silkscreen` enables it, while a new back-only set does not inherit a stale
front-side mirror. Disable Auto to keep a fully manual orientation. Both
controls are saved when the viewer closes.

With only PCB files loaded, the top action reads `Slice & Upload…`. It creates
the one-layer PWSZ, waits for successful Cloud parsing and delivers the file,
then stops without issuing a print order. The PCB tab exposes a separate
`Start uploaded PCB (platform removed)…` button. It uses `cloud-start-pcb` and
shows an explicit platform-removal/homing warning. Printer status, pause,
resume and safe active-project stop use the same Rust Cloud API as resin jobs.

Before an actual exposure:

1. inspect the complete layer for scale, polarity, clipped geometry, holes and
   mirror direction;
2. validate the PWSZ locally; `Slice & Upload…` can test transfer without
   sending any print/start command;
3. read printer status and require idle state;
4. remove the build platform before starting an arbitrary Gerber PWSZ, verify
   all remaining clearance, close the UV cover and prevent direct eye/skin
   exposure;
5. begin with a short calibration coupon, not a 60-second full board;
6. keep `cloud-stop <CONFIG> <PRINTER_ID>` ready and verify idle status after
   cancellation.

The separate built-in LCD service command has no print/Z fields and uses
firmware patterns 1, 2 or 4. It is useful for checking the screen without
platform motion, but the recovered protocol contains no filename, PWSZ or
bitmap field, so it cannot expose the imported Gerber artwork.

### Why RERF is not a no-motion custom LCD path

Photon Workshop's `Calibration Exposure`/RERF mode was checked separately
because firmware 4.0.8.6 advertises expanded 4/6/8-partition calibration.
The recovered implementation confirms that it is a specialised **sliced print
job**, not a service-screen bitmap upload:

* `FUN_1802cd2c0 @ 0x1802cd2c0` stores the partition count and
  `FUN_1802cdb30 @ 0x1802cdb30` stores the per-partition exposure-time array;
* `FUN_1802c96b0 @ 0x1802c96b0` prepares the RERF models and invokes
  `runSliceTask`;
* `FUN_1802cce60 @ 0x1802cce60` and
  `FUN_1802cd050 @ 0x1802cd050` invoke `saveSlicedFile` and
  `saveSlicedFileToTempFile` respectively;
* `lcd_func::Func_Rerf::ToJsonString @ 0x18002460e` serialises
  `rerf_function` metadata containing `enable`, `model_type`, `model_name`,
  `partition_num`, and `partition_exposure_array` into the sliced-file path.

This agrees with Anycubic's
[4.0.8.6 firmware notes](https://wiki.anycubic.com/en/resin-3d-printer/photon-mono-m7-pro/firmware-update-log):
the feature changes calibration partitions and their exposure times. It does
not document or expose a command that accepts arbitrary LCD pixels.

Consequently the recovered stock Workshop/Cloud/LAN API currently offers only
two choices: a normal PWSZ print that may perform mandatory initial Z
preparation, or a motion-free service exposure using one of the firmware's
built-in patterns. The project deliberately has no `custom-exposure` command:
implementing one from guessed fields or probing unknown `exposure_type` values
would be both unsupported and unsafe.

This does not prove that custom motion-free display is impossible inside the
printer. Resolving it requires the M7 Pro's **system/application firmware** (an
update package containing the root filesystem and printer daemons, or a
read-only dump), followed by analysis of the handler for exposure order 301
and the LCD/framebuffer or layer-decoder process. The separately published
`FPGA_1422_0047(0821).bin` bitstream is not that system firmware. Back up the
original storage before any firmware modification; no firmware image was
flashed during this work.

As a second check, Anycubic's official M7 Pro `R_E_R_F.pwsz` was inspected
without uploading it. It contains 288 ordinary layer images and requests an
8 mm Z lift on every layer; it is therefore particularly unsuitable as a
no-motion PCB exposure shortcut. The Cloud SDK does expose a possible route
to the missing system image: `FUN_1801f55a0 @ 0x1801f55a0` parses an OTA
`firmware_url`. The current 4.0.8.6 printer reports `need_update = 0`, however,
so the server response does not include that URL. A future OTA response, an
existing update cache, or a read-only eMMC/rootfs dump is needed to continue.

Rust callers can use the same path without the CLI:

```rust
use std::path::PathBuf;
use anycubicstudio::slicer::pcb::{PcbExposureConfig, slice_pcb};

let gerbers = vec![PathBuf::from("board-F_Cu.gbr")];
let drills = vec![PathBuf::from("board-PTH.drl")];
let mut config = PcbExposureConfig::default();
config.exposure_s = 60.0;
let report = slice_pcb(&gerbers, &drills, "board.pwsz", &config)?;
println!("{} exposed pixels", report.exposed_pixels);
# Ok::<(), anycubicstudio::Error>(())
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
Drag with the left mouse button to rotate, use Shift+left/middle/right drag to
pan, and use the wheel to zoom. Switch to
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
| `--support-penetration MM` | Distance by which a generated contact enters the model surface. |
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
| `--build-volume X,Y,Z` | Override the machine printing zone in millimetres. |
| `--move X,Y,Z` | Translation in millimetres, applied after centring/bed placement. |
| `--rotate X,Y,Z` | Euler rotations in degrees. |
| `--scale X,Y,Z` | Per-axis non-zero scale. A negative component mirrors that axis and repairs winding. |
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

### Multi-model project API

The GUI serializes a public `SceneProject`: one global `SlicerConfig` plus an
array of model paths, per-model transforms, and per-model support settings.
Host programs may construct the same type directly:

```rust
use anycubicstudio::slicer::{
    SceneModelConfig, SceneProject, SlicerConfig, SupportConfig, Transform,
    slice_project,
};

fn main() -> anycubicstudio::Result<()> {
    let mut left = Transform::default();
    left.translation_mm = [-35.0, 0.0, 0.0];

    let mut right = Transform::default();
    right.translation_mm = [35.0, 0.0, 0.0];
    right.scale = [-1.0, 1.0, 1.0]; // mirror X

    let mut medium = SupportConfig::medium();
    medium.enabled = true;
    let mut light = SupportConfig::light();
    light.enabled = true;

    let project = SceneProject {
        config: SlicerConfig::default(),
        models: vec![
            SceneModelConfig {
                input: "left.stl".into(),
                transform: left,
                supports: medium,
            },
            SceneModelConfig {
                input: "right.step".into(),
                transform: right,
                supports: light,
            },
        ],
    };
    let report = slice_project(&project, "plate.pwsz")?;
    println!("{} layers", report.layer_count);
    Ok(())
}
```

The equivalent non-interactive JSON commands used by the GUI are:

```bash
target/release/anycubicstudio project-viewer-scene plate.json plate.acv
target/release/anycubicstudio project-slice plate.json plate.pwsz
```

Models must not geometrically overlap. The slicer combines their already
positioned triangles before layer generation; the basic GUI `Auto layout`
action is intended to establish that invariant.

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

The program supports the same browser OAuth flow as Photon Workshop. It does
not accept or store an account password: credentials are entered only on the
Anycubic login page. To create or refresh a configuration:

```bash
cargo run -- cloud-login ./global_config.ini
```

The command opens the Anycubic page. After login, copy the final
`acpws://login?...` callback address (or only its `code`) back to the terminal.
For GUI use, select a config path and press `Log in / refresh account…`.

For scripts that open the browser separately:

```bash
cargo run -- cloud-login-url
printf '%s' '<ACPWS_CALLBACK>' | \
  cargo run -- cloud-login-complete ./global_config.ini
```

The resulting OAuth `accessToken` is saved with Unix mode `0600` where the
platform supports permissions. It is exchanged for a short-lived `XX-Token`:

```text
POST /v3/public/loginWithAccessToken
```

Typical file location on Windows:

```text
%LOCALAPPDATA%\ANYCUBIC\AnycubicPhotonWorkshop_V4.1.8\global_config.ini
```

An existing Workshop file is still accepted. When running from Linux with a
mounted Windows partition, provide its actual path. Do not copy
`global_config.ini` into the repository or expose its contents in logs.

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

For a one-layer PCB file, use the separate start mode only after removing the
build platform:

```bash
cargo run -- cloud-start-pcb \
  <PATH_TO_GLOBAL_CONFIG.INI> \
  <PRINTER_ID> \
  <CLOUD_FILE_ID>
```

This sends all confirmed editable M7 Pro preparation flags as zero, including
automatic resin feeding and heating. It does not and cannot disable
firmware-controlled homing or non-editable checks. The command also fails
closed unless Cloud reports the target printer online and in the observed idle
state.

To light only one of the firmware's built-in diagnostic patterns without
starting a print project or issuing a Z command:

```bash
cargo run -- cloud-exposure-start \
  <PATH_TO_GLOBAL_CONFIG.INI> <PRINTER_ID> <1|2|4> <SECONDS>

cargo run -- cloud-exposure-stop \
  <PATH_TO_GLOBAL_CONFIG.INI> <PRINTER_ID>
```

This diagnostic command cannot display a Gerber/PWSZ layer. The start command
is limited to 300 seconds by the client and refuses to run unless the target
printer is observed online and idle.

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
| `gui [MODEL ...]`                                 | Open the multi-model plate editor and controls   | Local GUI; buttons may invoke Cloud |
| `project-viewer-scene <JSON> <ACV>`               | Export a multi-model inspection scene            | Local file creation    |
| `project-slice <JSON> <PWSZ>`                     | Slice a serialized multi-model plate             | Local file creation    |
| `pcb-default-config <JSON>`                       | Write the default PCB exposure configuration      | Local file creation    |
| `pcb-viewer-scene <GERBER> <ACV> [PCB_OPTIONS]`   | Compose Gerber/DRL and export exact LCD preview    | Local file creation    |
| `pcb-slice <GERBER> <PWSZ> [PCB_OPTIONS]`         | Create a one-layer PCB photoresist exposure job    | Local file creation    |
| `cloud-login <CONFIG>`                            | Browser OAuth login; save token without collecting a password | Modifies local config |
| `cloud-login-url`                                 | Print Workshop's OAuth authorization URL          | Read-only              |
| `cloud-login-complete <CONFIG> [CALLBACK_OR_CODE]` | Exchange OAuth callback/code and save the token   | Modifies local config  |
| `cloud-status <CONFIG>`                           | List Cloud printers                             | Read-only              |
| `cloud-status-detail <CONFIG>`                    | List printers and detailed status               | Read-only              |
| `cloud-inspect <CONFIG>`                          | Show recent Cloud files and projects            | Read-only              |
| `cloud-file-status <CONFIG> <FILE_ID>`            | Read one uploaded file's parse state            | Read-only              |
| `cloud-slice-info <CONFIG> <GCODE_ID>`            | Read Cloud slice metadata for a parsed file      | Read-only              |
| `cloud-projects <CONFIG>`                         | Show recent projects and their states           | Read-only              |
| `cloud-upload <CONFIG> <PRINTER_ID> <FILE>`       | Upload and deliver a file without starting it   | Modifies Cloud storage |
| `cloud-start <CONFIG> <PRINTER_ID> <FILE_ID>`     | Start a file, `order_id=1`                      | Active command         |
| `cloud-start-pcb <CONFIG> <PRINTER_ID> <FILE_ID>` | Start a PCB file with confirmed editable preparation flags disabled | Active command |
| `cloud-exposure-start <CONFIG> <PRINTER_ID> <1\|2\|4> <SECONDS>` | Run a built-in LCD pattern, `order_id=301` | Active UV command |
| `cloud-exposure-stop <CONFIG> <PRINTER_ID>`       | Stop the built-in LCD test, `order_id=302`      | Active command         |
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
├── main.cpp         application startup
├── main_window.*    multi-model editor, slicer and Cloud workflow
├── print_view.*     OpenGL model/support/build-volume and layer renderer
└── scene.*          internal `.acv` scene parser and GUI project state

include/
└── anycubic_slicer.h    public C/C++ header
```

Reverse-engineering details, function addresses, and JSON packets are documented in `PROTOCOL.md`. Slicer format and PWSZ analysis are documented in `SLICER.md`.

## Known Limitations

* Browser OAuth authorization-code login is implemented, but secure long-term
  token storage currently relies on the permissions of the selected INI file,
  not an OS keychain.
* The Cloud API is undocumented by the manufacturer and may change.
* Cloud pause/resume codes have not been live-tested on the current M7 Pro.
* The test M7 Pro did not respond to Workshop SSDP, so Cloud communication was used instead.
* Generated PCB PWSZ files pass local validation and Anycubic Cloud parsing.
  A normal hardware start was accepted, but cancelled when its initial Z
  preparation moved the platform; the photoresist pattern itself has not yet
  been physically validated.
* `cloud-start-pcb` disables every editable preparation flag captured from
  Workshop, but the M7 Pro exposes levelling/platform/resin/residue checks as
  non-editable. No known PWSZ setting suppresses initial firmware homing.
* The motion-free `cloud-exposure-*` path supports only firmware patterns
  1, 2 and 4; the recovered request has no custom bitmap/PWSZ field.
* `calc_layer_volumes.data` has a confirmed container/record layout, but its adaptive M7 grouping algorithm is approximated with four-layer groups.
* Preview members now contain actual model/support renders at the expected 224×168, 336×252, and 800×600 dimensions. Their exact Workshop camera, lighting, and PNG compression are not byte-identical.
* Supports are independent vertical cylinder/tapered/square pillars. Workshop-compatible tree/branched supports, island-aware optimisation, cross-bracing, and exact VoxSDK preset parity are not implemented.
* GUI manipulation uses model selection plus numeric transform controls; an on-mesh transform gizmo, drag-to-place models, ray-picked support contacts, undo/redo, and project save/load are not implemented yet.
* GUI `Auto layout` is rectangular shelf packing from transformed bounds, not contour-aware nesting. It does not rotate parts to optimise plate usage.
* Hollowing, drain holes, mesh repair, and XY contour compensation are not implemented yet.
* The built-in 3MF reader handles Core meshes, build items, components, units, and transforms. It rejects files that declare required extensions; optional material and texture data are not used for monochrome resin slicing.
* The library does not schedule print jobs and does not replace the printer's hardware safety interlocks.

The project is not affiliated with Anycubic and uses an undocumented protocol. Before publishing or distributing it, check the applicable licenses and terms of service.
