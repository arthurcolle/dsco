// build.rs — wire the link path to the C runtime archive when it exists.
//
// Building the library alone does NOT require the archive: extern "C" symbols
// resolve only at final link (of a binary/test). This just makes the linkage
// ready once `make libdsco.a` has produced the archive, without ever failing
// a plain `cargo build` of the library.

use std::path::Path;

fn main() {
    // crate lives at <repo>/crates/dsco-core → repo root is two up.
    let root = Path::new(env!("CARGO_MANIFEST_DIR"))
        .parent()
        .and_then(|p| p.parent())
        .map(|p| p.to_path_buf());

    println!("cargo:rerun-if-changed=../../include/context_fabric.h");

    let Some(root) = root else { return };
    if root.join("libdsco.a").exists() {
        println!("cargo:rustc-link-search=native={}", root.display());
        println!("cargo:rustc-link-lib=static=dsco");
        // System libraries the C runtime pulls in (matches the Makefile LDLIBS).
        for lib in ["curl", "sqlite3", "z", "pthread", "m"] {
            println!("cargo:rustc-link-lib=dylib={lib}");
        }
        #[cfg(target_os = "macos")]
        for fw in [
            "Security", "CoreFoundation", "IOKit", "CoreGraphics", "CoreText",
            "LocalAuthentication", "Foundation", "Metal", "MetalKit", "Accelerate",
            "AudioToolbox",
        ] {
            println!("cargo:rustc-link-lib=framework={fw}");
        }
    }
}
