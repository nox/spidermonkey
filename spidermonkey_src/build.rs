use std::env;
use std::ffi::{OsStr, OsString};
use std::path::{Path, PathBuf};
use std::process::Command;
use std::str;

fn main() {
    let out_dir = PathBuf::from(env::var_os("OUT_DIR").unwrap());
    let cargo_manifest_dir = PathBuf::from(env::var_os("CARGO_MANIFEST_DIR").unwrap());
    let mozjs_dir = cargo_manifest_dir.join("mozjs");

    let build_dir = setup_build_directory(&out_dir);
    let python = setup_python_env(&out_dir.join("pycache"));

    setup_encoding_c_mem_env();

    configure(&python, &mozjs_dir, &build_dir);

    print_rerun_directives();
}

fn setup_build_directory(out_dir: &Path) -> PathBuf {
    let build_dir = out_dir.join("build");
    std::fs::create_dir_all(&build_dir).expect("could not create build directory");
    build_dir
}

fn setup_python_env(pycache_dir: &Path) -> PathBuf {
    fn parse_python_version(path: impl AsRef<OsStr>) -> Option<(usize, usize)> {
        let output = Command::new(path).arg("--version").output().ok()?;

        if !output.status.success() {
            return None;
        }

        if !output.stderr.is_empty() {
            return None;
        }

        if !output.stdout.starts_with(b"Python ") {
            return None;
        }
        let output = str::from_utf8(&output.stdout[b"Python ".len()..]).ok()?;
        let mut numbers = output.split('.');
        let major = numbers.next()?.parse().ok()?;
        let minor = numbers.next()?.parse().ok()?;
        Some((major, minor))
    }

    fn python_path() -> (PyVersion, PathBuf) {
        let vars = &["PYTHON3"];
        for var in vars {
            if let Some(python) = env::var_os(var) {
                match parse_python_version(&python) {
                    Some((major, minor)) if major == 3 => {
                        return (PyVersion::V3(minor), python.into());
                    }
                    _ => continue,
                }
            }
        }

        let commands = &["python3.8", "python3.7", "python3", "python"];
        for command in commands {
            if let Ok(python) = which::which(command) {
                match parse_python_version(&python) {
                    Some((major, minor)) if major == 3 => {
                        return (PyVersion::V3(minor), python.into());
                    }
                    _ => continue,
                }
            }
        }

        panic!("could not find a valid Python 3 interpreter");
    }

    #[derive(Debug)]
    enum PyVersion {
        V3(usize),
    }

    let (version, python) = python_path();
    let python = which::which(python).expect("could not get python path");
    env::remove_var("PYTHON");
    env::remove_var("PYTHON3");
    env::set_var("PYTHONNOUSERSITE", "1");
    match version {
        PyVersion::V3(minor) => {
            if minor >= 8 {
                env::set_var("PYTHONPYCACHEPREFIX", pycache_dir);
            } else {
                env::set_var("PYTHONDONTWRITEBYTECODE", "1");
            }
            env::set_var("PYTHON3", &python);
        }
    }
    python
}

fn setup_encoding_c_mem_env() {
    let encoding_c_mem_include_dir = env::var_os("DEP_ENCODING_C_MEM_INCLUDE_DIR").unwrap();
    let mut cppflags = OsString::from("-I");
    cppflags.push(encoding_c_mem_include_dir);
    cppflags.push(" ");
    cppflags.push(env::var_os("CPPFLAGS").unwrap_or_default());
    env::set_var("CPPFLAGS", cppflags);
}

fn configure(python: &Path, mozjs_dir: &Path, build_dir: &Path) {
    const DEFAULT_CONFIGURE_ARGS: &'static [&'static str] = &[
        "--build-backends=RecursiveMake",
        "--enable-project=js",
        "--disable-cranelift",
        "--disable-export-js",
        "--disable-gtest-in-build",
        "--disable-jemalloc",
        "--disable-jitspew",
        "--disable-js-shell",
        "--disable-nspr-build",
        "--disable-profiling",
        "--disable-shared-js",
        "--disable-tests",
        "--disable-trace-logging",
        "--disable-vtune",
        "--with-intl-api",
        "--without-system-zlib",
    ];

    let python = which::which(python).expect("could not find python path");
    let mut configure = Command::new(python);
    configure
        .current_dir(&build_dir)
        .env("OLD_CONFIGURE", mozjs_dir.join("js/src/old-configure"))
        .arg(mozjs_dir.join("configure.py"));
    for arg in DEFAULT_CONFIGURE_ARGS {
        configure.arg(arg);
    }
    assert!(configure.status().unwrap().success());
}

fn print_rerun_directives() {
    println!("cargo:rerun-if-env-changed=PYTHON3");
}
