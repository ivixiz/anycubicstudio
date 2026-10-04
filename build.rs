use std::{env, path::PathBuf};

fn occt_root() -> Option<PathBuf> {
    let mut candidates = Vec::new();
    if let Some(root) = env::var_os("OPENCASCADE_ROOT") {
        candidates.push(PathBuf::from(root));
    }
    // Local SDK supplied for development. Production/CI builds should set
    // OPENCASCADE_ROOT instead of relying on this machine-specific fallback.
    candidates.push(PathBuf::from("/home/dima/Documents/VSCode/C/opencascade"));
    candidates.push(PathBuf::from("/usr"));

    candidates.into_iter().find(|root| {
        root.join("include/opencascade/BRepMesh_IncrementalMesh.hxx")
            .is_file()
            && root
                .join("include/opencascade/STEPControl_Reader.hxx")
                .is_file()
            && root
                .join("include/opencascade/IGESControl_Reader.hxx")
                .is_file()
    })
}

fn main() {
    println!("cargo:rerun-if-changed=native/occt_tessellator.cpp");
    println!("cargo:rerun-if-env-changed=OPENCASCADE_ROOT");
    println!("cargo:rustc-check-cfg=cfg(anycubic_occt)");

    let target = env::var("TARGET").unwrap_or_default();
    let Some(root) = occt_root().filter(|_| target.contains("linux")) else {
        println!(
            "cargo:warning=complete OpenCascade development SDK not found; CAD import will use gmsh"
        );
        return;
    };
    let include = root.join("include/opencascade");
    let lib = root.join("lib");

    cc::Build::new()
        .cpp(true)
        .std("c++17")
        .warnings(true)
        .include(&include)
        .file("native/occt_tessellator.cpp")
        .compile("anycubic_occt_tessellator");

    println!("cargo:rustc-link-search=native={}", lib.display());
    // Keep binaries usable with a non-system OCCT SDK. Packaged builds can
    // override the root and set their install RPATH afterwards.
    if root.as_path() != std::path::Path::new("/usr") {
        // DT_RPATH is inherited while resolving OCCT's transitive libraries;
        // DT_RUNPATH is not, which breaks self-contained SDK installations.
        println!("cargo:rustc-link-arg=-Wl,--disable-new-dtags");
        println!("cargo:rustc-link-arg=-Wl,-rpath,{}", lib.display());
    }
    for library in [
        "TKDESTEP",
        "TKDEIGES",
        "TKDESTL",
        "TKDE",
        "TKXSBase",
        "TKShHealing",
        "TKMesh",
        "TKTopAlgo",
        "TKGeomAlgo",
        "TKBRep",
        "TKGeomBase",
        "TKG3d",
        "TKG2d",
        "TKMath",
        "TKernel",
    ] {
        println!("cargo:rustc-link-lib=dylib={library}");
    }
    println!("cargo:rustc-cfg=anycubic_occt");
    println!("cargo:warning=using OpenCascade SDK at {}", root.display());
}
