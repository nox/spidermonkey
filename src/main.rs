use std::fs;
use std::path::Path;
use std::process::Command;

fn main() {
    let cmd = std::env::args().nth(1).expect("no command");
    match &*cmd {
        "checkout" => checkout(),
        "format-patch" => format_patch(),
        "vendor" => vendor(),
        _ => panic!("unknown command"),
    }
}

fn checkout() {
    if !has_gecko_dev_dir() {
        println!("Creating Git repository…");
        fs::create_dir("gecko-dev").expect("could not create gecko-dev directory");
        let status = git("init").status().expect("could not run git-init");
        assert!(status.success(), "could not create git repo: {}", status);
    }

    println!("Fetching gecko-dev…");
    let status = git("fetch")
        .args(&["https://github.com/mozilla/gecko-dev.git", "release"])
        .status()
        .expect("could not run git-fetch");
    assert!(status.success(), "could not fetch gecko-dev: {}", status);

    let commit = gecko_dev_commit();
    println!("Checking out {}…", commit);
    let status = git("reset")
        .args(&["--hard", &commit])
        .status()
        .expect("could not run git-reset");
    assert!(
        status.success(),
        "could not check out {}: {}",
        commit,
        status
    );

    println!("Applying patches…");
    let mut patches = fs::read_dir("patches")
        .expect("could not open patches directory")
        .map(|entry| entry.expect("could not read entry").path())
        .filter(|path| path.is_file())
        .collect::<Vec<_>>();
    patches.sort_unstable();
    let status = git("am")
        .args(&["--3way", "--reject"])
        .args(patches.iter().map(|patch| Path::new("..").join(&patch)))
        .status()
        .expect("could not run git-am");
    assert!(status.success(), "could not apply patches");
}

fn format_patch() {
    if !has_gecko_dev_dir() {
        panic!("could not find gecko-dev clone");
    }

    println!("Formatting patches…");
    let commit = gecko_dev_commit();
    if Path::new("patches").is_dir() {
        fs::rename("patches", "patches.orig").expect("could not rename former patches directory");
    }
    let status = git("format-patch")
        .arg(format!("--base={}", commit))
        .args(&[
            "--output-directory",
            "../patches",
            "--no-stat",
            "--no-numbered",
            "--no-signature",
            "--zero-commit",
            &commit,
        ])
        .status()
        .expect("could not run git-format-patch");
    assert!(status.success(), "could not format patches");
}

fn vendor() {
    if !has_gecko_dev_dir() {
        panic!("could not find gecko-dev clone");
    }

    println!("Packaging mozjs…");
    let ref staging_dir = tempfile::Builder::new()
        .prefix("mozjs")
        .tempdir()
        .expect("could not create staging directory");
    let status = Command::new("gecko-dev/js/src/make-source-package.sh")
        .env("STAGING", staging_dir.path())
        .env("TAR", ":")
        .status()
        .expect("could not run make-source-package.sh");
    assert!(status.success(), "could not package mozjs");

    println!("Syncing mozjs…");
    let mozjs_dir = fs::read_dir(staging_dir.path())
        .expect("could not open staging directory")
        .map(|entry| entry.expect("could not read entry"))
        .find(|path| {
            path.file_name()
                .to_str()
                .map_or(false, |s| s.starts_with("mozjs-"))
        })
        .expect("could not find source directory")
        .path();
    let status = Command::new("rsync")
        .args(&[
            "--delete-excluded",
            "--filter=merge filters.txt",
            "--prune-empty-dirs",
            "--quiet",
            "--recursive",
        ])
        .arg(mozjs_dir.join("./"))
        .arg("spidermonkey_src/mozjs/")
        .status()
        .expect("could not run rsync");
    assert!(status.success(), "could not sync sources");
}

fn has_gecko_dev_dir() -> bool {
    Path::new("gecko-dev").is_dir()
}

fn git(action: &str) -> Command {
    let mut cmd = Command::new("git");
    cmd.args(&["-C", "gecko-dev", action]);
    cmd
}

fn gecko_dev_commit() -> String {
    let mut commit = fs::read_to_string("GECKO_DEV_COMMIT").expect("could not retrieve commit");
    let len = commit.trim_end().len();
    commit.truncate(len);
    commit
}
