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
| `step_iges_datachange_r.dll`  | confirmed/decompiled | OpenCascade STEP/IGES readers, shape repair/sewing, deterministic BRep tessellation, and flattened vertex/index export.                                                             |

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

The first sample is only 2,776 bytes despite an LCD resolution of 13312×5120. Tracing `FUN_180397720 @ 0x180397720`, nested serializer `FUN_180399060 @ 0x180399060`, polygon converter `FUN_180396dc0 @ 0x180396dc0`, and layer preparation `FUN_18039a4c0 @ 0x18039a4c0` establishes the binary representation below. All integers and floats are little-endian.

```text
u32  outer_magic          0x003d3d7b, bytes "{==\0"
f32  signed_twice_area    sum of contour shoelace products
f32  min_x
f32  min_y
f32  max_x
f32  max_y
u32  auxiliary_count      zero in the supplied file
u32  auxiliary[auxiliary_count]
u32  contour_count
u32  inner_magic          0x002d2d5b, bytes "[--\0"
u32  inner_reserved       zero in the supplied file
u32  segment_count
u32  encoding             1 for the binary encoding below

segment[segment_count]:   exactly 17 bytes per record
    f32 x1
    f32 y1
    f32 x2
    f32 y2
    u8  direction

u32  inner_footer         0x005d2d2d, bytes "--]\0"
u32  outer_footer         0x007d3d3d, bytes "==}\0"
```

For each oriented contour edge, the endpoint with the lower Y coordinate is stored first. `direction = 1` means this order is the original contour direction; `direction = 0` means the endpoints were swapped. Records are sorted by their lower Y coordinate. With an empty auxiliary vector the header before the first record is 48 bytes and total size is `56 + 17 * segment_count`.

The supplied file and the new writer both use millimetres around the centre of the platform. Outer contours are counter-clockwise and holes clockwise in the independent implementation; the signed shoelace sum therefore subtracts holes.

### `scene.slice`

`FUN_180342ad0 @ 0x180342ad0` writes a fixed 388-byte header:

```text
0x000  char[16] "ANYCUBIC-PWSZ\0\0\0"
0x010  char[64] "AnycubicPhotonWorkshop Exports\0..."
0x050  u32 3
0x054  u32 3
0x058  u32 0
0x05c  u32 0
0x060  f32 1.0
0x064  u32 layer_count
0x068  f32[6] scene bounds: min_x,min_y,min_z,max_x,max_y,max_z
0x080  u32[65] reserved/zero in the sample
```

It is followed by magic `0x2d2d2d3c` (`"<---"`), `u32 layer_count`, and one 64-byte record per layer. A record is `f32 z`, `f32 area`, four bounds, `u32 contour_count`, and nine zero/reserved words. The stream ends with `0x3e2d2d2d` (`"--->"`). `example.pwsz` is exactly `388 + 8 + 326*64 + 4 = 21,264` bytes.

### `calc_layer_volumes.data`

The container is partly recovered from `FUN_180358c80 @ 0x180358c80`: a 16-byte header (`u32 version`, `f32 precision`, `u32 enabled`, `u32 record_count`) followed by 48-byte records. In the supplied sample the header is `0x20251024`, `0.2`, `1`, `78`. The first fields of a record are a layer index and two floats; the final float behaves as grouped layer volume. The exact M7 Pro adaptive grouping policy is not yet proved. The independent writer emits deterministic four-layer groups with zero reserved fields, so this member remains the main printer-validation risk.

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

## Independent Rust Implementation

[`src/pwsz.rs`](src/pwsz.rs) reads the ZIP archive, strictly checks `layers_controller.count` against both the JSON array and the number of `layer_images`, and validates/decodes the recovered vector layer stream.

[`src/slicer/`](src/slicer/) contains a new implementation that does not call the proprietary Windows slicing engine:

```text
STL / mesh-3MF / STEP or IGES
  -> triangular mesh
  -> scale / rotate / move / centre / place on bed
  -> triangle-plane intersections at (layer + 0.5) * layer_height
  -> stitch and orient contours
  -> 17-byte PWSZ vector records
  -> metadata + scene.slice + PWSZ ZIP
```

STL and the Core mesh/build/component subset of 3MF are read directly. STEP and IGES are tessellated through the installed `gmsh` executable. The latter makes the Linux `.so` independent of the Windows MSVC ABI, but it is currently a runtime dependency for CAD input.

### Original STEP/IGES path confirmed from `step_iges_datachange_r.c`

The newly supplied decompilation resolves the major uncertainty around the
original CAD conversion path:

| Function | Address | Confirmed behaviour |
| --- | --- | --- |
| `StepIgesDataChange::StepIgesDataChange()` | `0x18000ded0` | Stores default linear deflection `0.1` and angular deflection `0.5`; initialises empty vertex/index vectors and the shape holder. |
| `StepIgesDataChange::OpenFile(std::string,int)` | `0x1800106a0` | Clears old mesh data, selects a CAD reader, transfers roots, then repairs/sews the resulting shape. The zero discriminator follows the STEP branch; the other branch constructs the IGES reader. |
| STEP reader constructor (`FUN_1803f7d60`) | `0x1803f7d60` | Constructs `STEPControl_Reader`. |
| IGES reader constructor (`FUN_18013e860`) | `0x18013e860` | Constructs `IGESControl_Reader`; also reads the OpenCascade `read.iges.onlyvisible` option. |
| shape repair (`FUN_180010ea0`) | `0x180010ea0` | Uses `ShapeFix_Shape`, then `BRepBuilderAPI_Sewing`, and returns the repaired/sewn `TopoDS_Shape`. |
| `StepIgesDataChange::read()` | `0x180011fc0` | Calls `BRepMesh_IncrementalMesh(shape, 0.1, false, 0.5, false)`, iterates `TopoDS_Face` values, extracts triangulations, and builds flat vertex/index arrays. |
| `StepIgesDataChange::getVertexs()` | `0x180011fa0` | Returns `vector<tuple<float,float,float>>` by value. |
| `StepIgesDataChange::getIndices()` | `0x180011f80` | Returns `vector<unsigned int>` by value. |
| `StepIgesDataChange::set_tolerance(double,double)` | `0x180012b10` | Replaces the two deflection values used by `read()`. |

The `BRepMesh_IncrementalMesh` call is visible directly at
`step_iges_datachange_r.c` lines around 20550. Its recovered signature matches
OpenCascade's shape, linear deflection, relative flag, angular deflection, and
parallel flag ordering. Therefore `0.1` and `0.5` are not unit conversion
constants: they are absolute chordal and angular meshing tolerances. The
angular value is in OpenCascade's native radians.

The current independent implementation still uses Gmsh rather than loading the
proprietary Windows DLL or exposing its MSVC C++ ABI. Gmsh uses its own CAD
mesher, so identical STEP triangles are not claimed even though final geometry
and PWSZ layers are deterministic. Exact original tessellation on Linux would
require a small native OpenCascade adapter using the confirmed call above (and
the OCCT data-exchange development headers), or an in-memory mesh ABI from the
host application.

The supplied `MDP-P906.step` test produced:

```text
3,746 triangles
1,314 layers at 0.05 mm
bounds 100.700 × 13.500 × 65.680 mm
estimated resin volume 14.997 ml
1,324 valid ZIP members
```

All 1,314 layer members passed magic, count, record-size, and footer validation. `scene.slice` has the expected exact length and delimiters. This proves structural self-consistency, not yet acceptance or printing by M7 Pro firmware.

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

The independent Rust implementation now provides a deliberately smaller,
documented support subsystem. It samples downward-facing triangles on an XY
grid, finds the highest model surface below each contact (falling back to the
build plate), filters supports by minimum/maximum height, and emits closed
tiered solids. Cylinder, tapered, and square shapes plus light/medium/heavy
presets and explicit manual contact/base points are exposed through CLI, JSON,
Rust, and the stable C ABI. Model and support meshes are sliced separately and
their oriented polygons are merged at the layer boundary.

This is an implementation fact about this repository, not evidence that
Workshop uses the same contact selection. The decompiled VoxSDK proves a much
broader system (point/bar/line/web/solid/base-plate and tree supports), and the
field-level preset structures/branching heuristics remain unrecovered.

The decompiled DLLs confirm a specific set of geometry operations:

* `VoxSDKTriangleHollow.dll` exports `VoxApi::TriangleHollow{,WithFilter}::{hollow,hollowWithLattice}` and `setFilterSmallShells`. Parameters include thicknesses, filters, and a callback. The numeric values/enums of `LatticeType` and `FilterType` have not yet been recovered.

* `AcPerforateApi::Perforation` provides `setMesh`, `addHole`, `addHoles`, `perforate`, `perforate_batch`, `revoke`, `redo`, and command history. This is the API used by `FUN_180156680`; its `HoleParas` structure still needs field-level reconstruction before it can be ported to Rust.

* `VoxApi::SupportGeneratorBar::generateSupport` and `SupportGeneratorBase::generateSupport` in `VoxSDKSupportGeneration.dll` return/populate `SupportKernel`. Internally, there are `Point`, `Bar`, `Line`, `Web`, `Solid`, `BasePlate`, `LowestPoint`, and `LowestEdge` generators, overhang/anchor detection stages, and `generateSupportPreview`.

  This proves the available capabilities, but not a stable ABI: all APIs use the MSVC C++ ABI with parameter structures that have not yet been reconstructed.

* `VoxSDKTriangleFix.dll` provides `automaticFix`, `fixHoles`, `fixNormal`, `stitchTriangles`, `removeNoiseShells`, and evaluators for holes, intersections, non-manifold geometry, and orientation.

  `VoxSDKTriangleEdit.dll` contains `PlaneCut::cut`, `Shell2Parts::convert`, and a separate `Perforation` implementation.

## What Still Needs to Be Investigated or Decompiled

No additional DLL is required to run the current independent default slicer. The following evidence would reduce compatibility risk or is required for feature parity:

1. Trace all fields populated in `ModelCalcLayerVolumeInfo` before `FUN_180358c80 @ 0x180358c80`, plus reader `FUN_180351040 @ 0x180351040`. This is needed to reproduce the firmware's adaptive grouping policy rather than the current deterministic approximation.

2. Supply one fresh PWSZ made by Workshop **4.1.8** from the same `MDP-P906.step`, with supports/hollow/offset disabled and the documented default resin. The present sample identifies itself as Workshop 4.1.5. A matched file allows layer-by-layer differential validation.

3. Supply small files exercising one feature at a time: one hole, one hollow shell, one manual support, one automatic support, and positive/negative XY compensation. These fixtures are more useful than more bulk decompilation for observable-output tests.

4. For exact support/hollow parity, continue field reconstruction of `VoxSDKSupportGeneration.dll`, `TreeSupportLib.dll`, `VoxSDKTriangleHollow.dll`, and `AcPerforate.dll`. Their exported MSVC C++ structures are not stable ABIs, so an open implementation can instead use independent algorithms while preserving the final PWSZ contract.

5. `mesh_0.vf`, `node_0.nd`, and the project writer are needed only for compatible editable `.pwscene` output, not for printable PWSZ generation.

## Architectural Implications for Rust and Bambu Studio

The Rust network client remains independent from the slicer.

The new library builds both an `rlib` and `libanycubicstudio.so` and exports a small C ABI in [`include/anycubic_slicer.h`](include/anycubic_slicer.h). This is the intended Bambu Studio integration boundary. The proprietary core remains unsuitable for direct linkage because its interface uses the MSVC C++ ABI.

The current contour slicer is intentionally independent. If Bambu Studio geometry/support code is copied rather than merely called through a boundary, its AGPL-3.0 licensing obligations must be handled by the distributor.
