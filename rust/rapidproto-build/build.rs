// Picks the generator backend. With the `native` feature, compile the generator's C++ (the
// repository's src/ and include/) into a static library the crate calls in-process -- always
// optimized, whatever the consumer's profile, since an unoptimized generator is what made large
// schemas slow. If that fails and the `wasm` feature is on, fall back to the embedded wasm generator
// with a warning; with `native` alone, fail the build. The chosen backend is the `rp_native` cfg.

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
        Err(e) if cfg!(feature = "wasm") => println!(
            "cargo::warning=rapidproto-build: the native generator did not build ({e}); using the \
             wasm generator, which is slow on large schemas unless build scripts are optimized (see \
             the rapidproto-build docs)"
        ),
        Err(e) => panic!("rapidproto-build: the native generator did not build: {e}"),
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
        if let Some(missing) = files.iter().find(|f| !f.is_file()) {
            return Err(format!("generator source {} not found", missing.display()));
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
            // The generator's warnings are the project's to fix, not the consumer's to read.
            .warnings(false)
            .cargo_warnings(false);
        // MSVC needs exception handling switched on (native_entry.cpp catches at the C ABI).
        if env::var("CARGO_CFG_TARGET_ENV").is_ok_and(|e| e == "msvc") {
            build.flag("/EHsc");
        }
        build
            .try_compile("rapidproto_rustgen")
            .map_err(|e| e.to_string())
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
