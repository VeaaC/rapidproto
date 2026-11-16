// Picks the generator backend. With the `native` feature, compile the generator's C++ (the copy in
// cpp/, written by wasm/build-wasm.sh) into a static library the crate calls in-process, always
// optimized, whatever the consumer's profile. If that fails and the `wasm` feature is on, use the
// embedded wasm generator instead, handing the reason to the crate (`RAPIDPROTO_BUILD_FALLBACK`) so
// it can warn from the consumer's build script, where cargo shows warnings; with `native` alone,
// fail the build. The chosen backend is the `rp_native` cfg.

#[cfg(not(any(feature = "native", feature = "wasm")))]
compile_error!(
    "rapidproto-build needs a generator: enable the `native` feature, `wasm`, or both (the default)"
);

fn main() {
    println!("cargo::rustc-check-cfg=cfg(rp_native)");
    #[cfg(feature = "native")]
    match native::compile() {
        Ok(()) => println!("cargo::rustc-cfg=rp_native"),
        Err(reason) if cfg!(feature = "wasm") => {
            println!(
                "cargo::rustc-env=RAPIDPROTO_BUILD_FALLBACK={}",
                reason.summary
            );
            // Shown only when this crate is built from a path (in this repository, say).
            println!(
                "cargo::warning=the native generator did not build, using the wasm one: {}",
                reason.detail
            );
        }
        Err(reason) => panic!(
            "rapidproto-build: the native generator did not build: {}",
            reason.detail
        ),
    }
}

#[cfg(feature = "native")]
mod native {
    use std::env;
    use std::path::{Path, PathBuf};

    /// Why the native generator did not build: `summary` for the consumer's warning, with what
    /// would fix it; `detail` for this crate's own build output.
    pub struct Failure {
        pub summary: String,
        pub detail: String,
    }

    pub fn compile() -> Result<(), Failure> {
        let cpp = PathBuf::from(env::var("CARGO_MANIFEST_DIR").unwrap()).join("cpp");
        println!("cargo::rerun-if-changed={}", cpp.display());
        let mut files = Vec::new();
        translation_units(&cpp.join("src"), &mut files).map_err(|e| Failure {
            summary: "its C++ sources are missing from this copy of rapidproto-build".into(),
            detail: format!("cannot read {}: {e}", cpp.display()),
        })?;
        files.sort();

        let mut build = cc::Build::new();
        build
            .cpp(true)
            .std("c++17")
            .opt_level(2)
            .debug(false)
            .define("NDEBUG", None)
            .include(cpp.join("include"))
            .files(&files)
            // No warnings for the consumer to read; a failing compile still shows its errors.
            .warnings(false);
        let compiler = build.try_get_compiler().map_err(|e| Failure {
            summary: "the C++ compiler could not be set up".into(),
            detail: e.to_string(),
        })?;
        // The flags the project's own MSVC build uses: standard conformance, exception handling
        // (native_entry.cpp catches at the C ABI), UTF-8 sources.
        if compiler.is_like_msvc() {
            build.flag("/permissive-").flag("/EHsc").flag("/utf-8");
        }
        // A missing compiler fails here too: cc only guesses at one it cannot find.
        build
            .try_compile("rapidproto_rustgen")
            .map_err(|_| Failure {
                summary: "no C++17 compiler could build it: install one, then run `cargo clean -p \
                      rapidproto-build`"
                    .into(),
                detail: "compiling it failed (the compiler's output is above)".into(),
            })
    }

    fn translation_units(dir: &Path, files: &mut Vec<PathBuf>) -> std::io::Result<()> {
        for entry in std::fs::read_dir(dir)? {
            let path = entry?.path();
            if path.is_dir() {
                translation_units(&path, files)?;
            } else if path.extension().is_some_and(|e| e == "cpp") {
                files.push(path);
            }
        }
        Ok(())
    }
}
