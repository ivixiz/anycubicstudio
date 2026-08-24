# Reconstructing the Anycubic Photon Workshop 4.1.8 SLA Slicer

This document separates facts confirmed through Ghidra from working conclusions and still-unknown parts of the format. The addresses refer to `wrapper_slicer_engine_r.dll` or `mainwindow.dll` from version 4.1.8 x64. Container validation was performed using `example.pwsz` and `example.pwscene`; their `software_info.conf` identifies Workshop 4.1.5, so byte-level compatibility with 4.1.8 cannot be considered proven without a second sample. 

## Component Roles

| Component                     | Status               | Role                                                                                                                                                                               |
| ----------------------------- | -------------------- | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `wrapper_slicer_engine_r.dll` | confirmed            | Main C++ core: scene/meshes, machine and resin profiles, parallel slicing, raster layers, print estimation, and format writing. Exports 1,424 named C++ symbols.                   |
| `mainwindow.dll`              | confirmed            | Orchestrates the UI and the complete slicing pipeline; imports the core and hollow/support/hole libraries.                                                                         |
| `facegen.dll`                 | confirmed            | Not a slicer. Uses only `deduplicate_verts_funcs::convert_trimesh` and `deduplicate_verts_funcs::deduplicate_trimesh` from the core. Used for geometry/representation preparation. |
| `printer.dll`                 | previously confirmed | Printer models, LCD/resin/layer parameters, and JSON profiles.                                                                                                                     |
| `TreeSupportLib.dll`          | confirmed            | Local tree-support scene: exports `TreeSupportLibExport::Instance` and `clearSupportScene`; the instance is created from `mainwindow.dll`.                                         |
| `VoxSDKTriangleHollow.dll`    | confirmed by calls   | Model hollowing and lattice generation.                                                                                                                                            |
| `AcPerforate.dll`             | confirmed by calls   | Drain holes.                                                                                                                                                                       |
| `VoxSDKSupportGeneration.dll` | confirmed            | Automatic and manual point/bar/line/web/solid/base-plate supports; exported names reveal individual generators and progress callbacks.                                             |
| `VoxSDKTriangleFix.dll`       | confirmed            | Mesh inspection and repair: holes, normals, non-manifold geometry, intersections, shells, and noise shells.                                                                        |
| `VoxSDKTriangleEdit.dll`      | confirmed            | Plane cutting, shell separation, and a second perforation API.                                                                                                                     |
| `VoxSDKTriangleLayout2D.dll`  | confirmed            | 2D/2.5D nesting and automatic placement on the build platform.                                                                                                                     |

`wrapper_slicer_engine_r.dll` depends only on Qt5Gui/Qt5Core, bcrypt, and system libraries. This is a good indication that the format handling and slicing itself are implemented there rather than hidden inside a network or cloud module.

## Confirmed Pipeline

In `mainwindow.dll`, `FUN_1802c0500 @ 0x1802c0500`, the application creates an `eigen_scene_slicer`, configures its profile, and starts processing:

```text
scene / imported mesh / transformations
  -> MachineLcd + resin parameters + layer parameters
  -> eigen_scene_slicer::parallel_slice
  -> eigen_scene_slicer::calc_print_info
  -> raster layers and preview
  -> eigen_scene_slicer::compress_slicedimages
  -> eigen_scene_slicer::save_file
```

Key core functions:

| Function                                    | Address                     | Confirmed purpose                                     |
| ------------------------------------------- | --------------------------- | ----------------------------------------------------- |
| `eigen_scene_slicer::set_machine`           | `0x180024208`               | Supplies the LCD/machine profile.                     |
| `eigen_scene_slicer::set_resinpara`         | `0x18000e769`               | Supplies resin parameters.                            |
| `eigen_scene_slicer::set_layerspara`        | `0x18000baff`               | Supplies layer parameters.                            |
| `eigen_scene_slicer::set_callback`          | near the setup stage        | Progress/callback handling.                           |
| `eigen_scene_slicer::parallel_slice`        | `0x180014b0a`               | Parallel layer calculation.                           |
| `eigen_scene_slicer::calc_print_info`       | called from `FUN_1802c0500` | Calculates print information such as time and volume. |
| `eigen_scene_slicer::compress_slicedimages` | `0x18000a9ed`               | Compresses prepared layers.                           |
| `eigen_scene_slicer::save_file`             | `0x180004d4a`               | Writes the result.                                    |

Preview images are obtained through `get_raster_image` and `raster_gray_image::post_process` (`mainwindow.c`, around line 482030).

The core recognizes STL, OBJ, OFF, and PLY input meshes, as well as `z3mf` project types.

It contains handlers for `pws_file_base`, `pwsz_file_base`, `photon_file_base`, `photons_file_base`, and `chitu_gf_file` output formats. The `CreateInstance::newSlicedFile @ 0x18001f465` factory selects a registered handler based on a string type/suffix. The mapping between a specific printer profile and its output extension has not yet been proven and should not be guessed.

## PWSZ Format: Corrected and Verified Findings

The previous hypothesis about an external header was disproven by an actual file:

`example.pwsz` starts with `PK 03 04` and opens as a **standard deflate-compressed ZIP archive**. This fully agrees with `pwsz_file_base::open_file @ 0x180010410`, `SlicedImage::closeZipHandle @ 0x18000810c`, and the writer's ZIP operations.

The `ANYCUBIC-PWSZ\0` signature does exist, but it is located inside the `scene.slice` member rather than at offset zero of the outer file.

The first 80 bytes of `scene.slice` look like this:

```text
0x00..0x0f  ASCII "ANYCUBIC-PWSZ\0" (16 bytes)
0x10..0x4f  ASCII "AnycubicPhotonWorkshop Exports\0" (64 bytes)
0x50...     binary scene fields; semantics not yet established
```

This clarifies the purpose of `FUN_180342ad0 @ 0x180342ad0`: it creates the internal `scene.slice`, not the outer container.

Confirmed internal data names:

| Data                             | Code                                                                     |
| -------------------------------- | ------------------------------------------------------------------------ |
| `layers_controller.conf`         | `pwsz_file_base::write_layers_paras @ 0x18001b4f5`                       |
| `anycubic_photon_resins.pwsp`    | `FUN_180356740 @ 0x180356740`                                            |
| `print_info.json`                | `FUN_180356740`                                                          |
| `software_info.conf`             | `FUN_180356740`                                                          |
| `preview_images/preview_<n>.png` | `FUN_180357d60 @ 0x180357d60`                                            |
| `lcd_function.json`              | `FUN_180358230 @ 0x180358230`                                            |
| `layer_images/layer_<n>.pwszImg` | `FUN_180357360 @ 0x180357360`; extension confirmed by profile and sample |
| `calc_layer_volumes.data`        | `FUN_180358c80 @ 0x180358c80`                                            |
| `scene.slice`                    | `FUN_180358c80`                                                          |

`example.pwsz` contains 336 ZIP entries: 326 `pwszImg` layers and three PNG previews.

Its active profile is **Anycubic Photon Mono M7 Pro**:

```text
key_suffix        = pwsz
key_image_format  = pwszImg
LCD               = 13312 × 5120
build volume      = 223.642 × 126.480 × 230.000 mm
```

`layers_controller.conf` is JSON containing the exact layer count and a `paras` array. For this sample, it contains 326 entries with `layer_index`, `layer_minheight`, `layer_thickness`, `exposure_time`, `zup_height`, and `zup_speed`.

`print_info.json` reports 1,771 seconds, 1.930624 ml, and 2.316749 g.

Native `pwszImg` is neither PNG nor a raw LCD framebuffer.

The first sample is only 2,776 bytes despite an LCD resolution of 13312×5120. In all inspected layers, the first 48 bytes are little-endian; in the first layer this starts with `7b 3d 3d 00` (`"{==\0"`), followed by four `f32` values at offsets `0x04..0x13`, then 28 bytes of fields that have not yet been identified.

The subsequent stream contains coordinate-like `f32` values and byte tags.

This is consistent with a segmented/vector representation, but the record types, filling rules, and rendering procedure are **not yet proven**. The new Rust implementation therefore intentionally exposes this data as raw bytes rather than presenting a hypothesis as a decoder.

## PWSCENE Format

`example.pwscene` is also a regular deflate-compressed ZIP archive. It contains:

```text
meshes/mesh_0.vf
mainnodes/node_0.nd
preview/image_0.png, preview/image_1.png
config/anycubic_photon_resins.pwsp
config/anycubic_resin_names.json
config/custom_resin_names.json
multi_plates.bin
header
```

The internal `header` begins with `ANYCUBIC-SCENE\0`, followed by a build string.

`mesh_0.vf`, `node_0.nd`, and `multi_plates.bin` are binary; their field-level specifications have not yet been established.

This is an editable project format rather than a print-job file, so it is not required for either uploading or the PWSZ reader.

## Rust Implementation of the Verified Subset

The [`src/pwsz.rs`](src/pwsz.rs) module was added. `PwszArchive::open` reads the ZIP archive, strictly checks `layers_controller.count` against both the JSON array and the number of `layer_images`, and exposes the profile, metadata, previews, and raw layers.

Verification using an actual sample:

```text
cargo run --offline -- pwsz ../example.pwsz

machine: Anycubic Photon Mono M7 Pro
LCD: 13312x5120
job: 326 layers, 3 previews, 336 ZIP entries, estimated Some(1771) s
```

## Hollowing, Holes, and Supports

* `AppWorker::hollowModelWithFilter` in `mainwindow.c` around lines 96631–96904 calls `VoxApi::TriangleHollowWithFilter::hollow` and `hollowWithLattice`.

* `FUN_180156680 @ 0x180156680` uses `AcPerforateApi::Perforation::{setMesh,addHole,setTrianglesProp,perforate,clearTrianglesProp}`. This unambiguously associates `AcPerforate.dll` with drain-hole generation.

* `AnycubicSupport::TreeSupportLibExport::Instance` is created in `mainwindow.c` around lines 280663 and 283769.

The decompiled DLLs confirm a specific set of geometry operations:

* `VoxSDKTriangleHollow.dll` exports `VoxApi::TriangleHollow{,WithFilter}::{hollow,hollowWithLattice}` and `setFilterSmallShells`. Parameters include thicknesses, filters, and a callback. The numeric values/enums of `LatticeType` and `FilterType` have not yet been recovered.

* `AcPerforateApi::Perforation` provides `setMesh`, `addHole`, `addHoles`, `perforate`, `perforate_batch`, `revoke`, `redo`, and command history. This is the API used by `FUN_180156680`; its `HoleParas` structure still needs field-level reconstruction before it can be ported to Rust.

* `VoxApi::SupportGeneratorBar::generateSupport` and `SupportGeneratorBase::generateSupport` in `VoxSDKSupportGeneration.dll` return/populate `SupportKernel`. Internally, there are `Point`, `Bar`, `Line`, `Web`, `Solid`, `BasePlate`, `LowestPoint`, and `LowestEdge` generators, overhang/anchor detection stages, and `generateSupportPreview`.

  This proves the available capabilities, but not a stable ABI: all APIs use the MSVC C++ ABI with parameter structures that have not yet been reconstructed.

* `VoxSDKTriangleFix.dll` provides `automaticFix`, `fixHoles`, `fixNormal`, `stitchTriangles`, `removeNoiseShells`, and evaluators for holes, intersections, non-manifold geometry, and orientation.

  `VoxSDKTriangleEdit.dll` contains `PlaneCut::cut`, `Shell2Parts::convert`, and a separate `Perforation` implementation.

## What Still Needs to Be Investigated

All requested DLLs are already available, so no additional decompilation is required for a minimal PWSZ uploader/inspector. The next steps involve tracing specific functions rather than searching for more files:

1. Implementations of the `pwszImg` virtual methods selected by `CreateInstance::newSlicedImage @ 0x1800041f6`, to precisely describe the stream following the 48-byte header.

2. `FUN_180358c80 @ 0x180358c80` and reader `FUN_180351040 @ 0x180351040`, to reconstruct the binary `calc_layer_volumes.data` and `scene.slice` formats.

3. `mesh_0.vf` / `node_0.nd` and the project writer, but only if compatible `.pwscene` writing is required. This is independent from generating a printable file.

4. New files produced by Workshop 4.1.8 and by a different printer model, to distinguish the general PWSZ specification from M7 Pro-specific profile details.

## Architectural Implications for Rust and Bambu Studio

The Rust network client should remain independent from the slicer.

Temporary compatibility with the proprietary slicing core is possible only through a small C++ adapter or a separate process: the exported interface of `wrapper_slicer_engine_r` uses the MSVC C++ ABI rather than a stable C ABI suitable for direct Rust FFI.

For an open implementation, the better approach is to reproduce the slicing pipeline and writer after validating PWSZ against real files.

Bambu Studio components can be considered as a source of ideas/code for an open SLA slicing pipeline, but its AGPL-3.0 license must be taken into account before integration.
