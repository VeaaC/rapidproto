// Picks the generator backend. With the `native` feature, compile the generator's C++ (the
// repository's src/ and include/) into a static library the crate calls in-process, always
// optimized, whatever the consumer's profile. If that fails and the `wasm` feature is on, use the
// embedded wasm generator instead, handing the reason to the crate (`RAPIDPROTO_BUILD_FALLBACK`) so
// it can warn from the consumer's build script, where cargo shows warnings; with `native` alone,
// fail the build. The chosen backend is the `rp_native` cfg.

fn main() {
    println!("cargo::rustc-check-cfg=cfg(rp_native)");
    #[cfg(not(feature = "native"))]
    assert!(
        cfg!(feature = "wasm"),
        "rapidproto-build needs a generator: enable the `native` feature, `wasm`, or both (the default)"
    );
    #[cfg(feature = "native")]
    match native::compile() {
        Ok(()) => println!("cargo::rustc-cfg=rp_native"),
        Err(reason) if cfg!(feature = "wasm") => {
            println!("cargo::rustc-env=RAPIDPROTO_BUILD_FALLBACK={reason}");
        }
        Err(reason) => panic!("rapidproto-build: the native generator did not build: {reason}"),
    }
}

#[cfg(feature = "native")]
mod native {
    use std::env;
    use std::path::{Path, PathBuf};

    pub fn compile() -> Result<(), String> {
        let manifest = PathBuf::from(env::var("CARGO_MANIFEST_DIR").unwrap());
        let repo = manifest.join("../..");
        let list = manifest.join("generator-sources.txt");
        let sources = std::fs::read_to_string(&list)
            .map_err(|e| format!("cannot read {}: {e}", list.display()))?;
        let mut files: Vec<PathBuf> = sources
            .lines()
            .map(str::trim)
            .filter(|l| !l.is_empty() && !l.starts_with('#'))
            .map(|l| repo.join(l))
            .collect();
        files.push(repo.join("src/rustgen/native_entry.cpp"));
        // The sources sit in the repository around this crate, so a build outside a checkout (a
        // packaged crate) has none.
        if files.iter().any(|f| !f.is_file()) {
            return Err(
                "its C++ sources are not here (not built from a RapidProto checkout)".into(),
            );
        }

        let out = PathBuf::from(env::var("OUT_DIR").unwrap());
        let gen_include = out.join("gen-include");
        write_version_header(&repo, &gen_include)?;

        println!("cargo::rerun-if-changed={}", list.display());
        println!("cargo::rerun-if-changed={}", repo.join("include").display());
        for file in &files {
            println!("cargo::rerun-if-changed={}", file.display());
        }

        let mut build = cc::Build::new();
        build
            .cpp(true)
            .std("c++17")
            .opt_level(2)
            .debug(false)
            .define("NDEBUG", None)
            .include(repo.join("include"))
            .include(&gen_include)
            .files(&files)
            // No warnings for the consumer to read; a failing compile still shows its errors.
            .warnings(false);
        let compiler = build
            .try_get_compiler()
            .map_err(|_| "no C++ compiler found".to_string())?;
        // The flags the project's own MSVC build uses: standard conformance, exception handling
        // (native_entry.cpp catches at the C ABI), UTF-8 sources.
        if compiler.is_like_msvc() {
            build.flag("/permissive-").flag("/EHsc").flag("/utf-8");
        }
        build
            .try_compile("rapidproto_rustgen")
            .map_err(|_| "compiling it failed".to_string())
    }

    // The one CMake-generated header the generator includes, rapidproto/version.hpp, made the way
    // CMake and wasm/build-wasm.sh make it: cmake/version.hpp.in with the VERSION of project() in
    // CMakeLists.txt.
    fn write_version_header(repo: &Path, gen_include: &Path) -> Result<(), String> {
        let read = |p: PathBuf| {
            println!("cargo::rerun-if-changed={}", p.display());
            std::fs::read_to_string(&p).map_err(|e| format!("cannot read {}: {e}", p.display()))
        };
        let cmake = read(repo.join("CMakeLists.txt"))?;
        let version = cmake
            .lines()
            .filter(|l| l.starts_with("project("))
            .find_map(|l| l.split("VERSION ").nth(1))
            .and_then(|rest| {
                rest.split(|c: char| !(c.is_ascii_digit() || c == '.'))
                    .next()
            })
            .filter(|v| !v.is_empty())
            .ok_or("no project(... VERSION ...) in CMakeLists.txt")?;
        let header = read(repo.join("cmake/version.hpp.in"))?.replace("@PROJECT_VERSION@", version);
        let dir = gen_include.join("rapidproto");
        std::fs::create_dir_all(&dir).map_err(|e| e.to_string())?;
        std::fs::write(dir.join("version.hpp"), header).map_err(|e| e.to_string())
    }
}
