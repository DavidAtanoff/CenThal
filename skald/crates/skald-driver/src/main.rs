//! `skaldc` CLI entry point.

use std::path::PathBuf;
use std::process::ExitCode;

use skald_driver::{render_report, Driver};

fn main() -> ExitCode {
    let args: Vec<String> = std::env::args().collect();
    if args.len() < 2 {
        eprintln!("usage: skaldc <file-or-dir> [<file-or-dir> ...]");
        eprintln!("       skaldc --check <file-or-dir>   (frontend check only)");
        return ExitCode::from(2);
    }

    let mut driver = Driver::new();
    let mut any_errors = false;

    for arg in &args[1..] {
        if arg == "--check" || arg == "--help" || arg == "-h" {
            continue;
        }
        let path = PathBuf::from(arg);
        if path.is_dir() {
            let reports = driver.process_dir(&path);
            for r in &reports {
                print!("{}", render_report(r));
                if r.has_errors() { any_errors = true; }
            }
        } else if path.is_file() {
            let src = std::fs::read_to_string(&path).unwrap_or_default();
            let r = driver.process_file(&path, &src);
            print!("{}", render_report(r));
            if r.has_errors() { any_errors = true; }
        } else {
            eprintln!("skaldc: not found: {}", path.display());
            any_errors = true;
        }
    }

    if any_errors {
        ExitCode::from(1)
    } else {
        ExitCode::from(0)
    }
}
