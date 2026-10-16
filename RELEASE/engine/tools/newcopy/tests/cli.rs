//! Runs the `newcopy` program on fake Steam folders and installs.

use game_sources::testkit::{fake_install, write_file, TempDir};
use std::fs;
use std::path::Path;
use std::process::{Command, Output};

/// Runs newcopy so the player's real Steam is never looked at: Steam is
/// "at" a folder that doesn't exist (which also skips the Windows registry)
/// unless the test passes --steam, and HOME points into the test's folder.
fn newcopy(home: &Path, args: &[&str]) -> Output {
    Command::new(env!("CARGO_BIN_EXE_newcopy"))
        .args(args)
        .env("HOME", home)
        .env_remove("XDG_DATA_HOME")
        .env("GAME_SOURCES_STEAM", home.join("no steam here"))
        .output()
        .expect("newcopy runs")
}

fn text(output: &Output) -> String {
    format!(
        "{}{}",
        String::from_utf8_lossy(&output.stdout),
        String::from_utf8_lossy(&output.stderr)
    )
}

fn fake_steam(steam: &Path) {
    fake_install(&steam.join("steamapps/common/Call of Duty 2"), "cod2").unwrap();
    write_file(
        steam,
        "steamapps/appmanifest_2630.acf",
        b"\"AppState\"\n{\n\t\"appid\"\t\t\"2630\"\n\t\"installdir\"\t\t\"Call of Duty 2\"\n}\n",
    )
    .unwrap();
}

#[test]
fn find_make_check_from_steam() {
    let tmp = TempDir::new("cli").unwrap();
    let steam = tmp.path().join("Steam");
    fake_steam(&steam);
    let steam_arg = steam.to_str().unwrap();

    let found = newcopy(tmp.path(), &["find", "--steam", steam_arg]);
    let found_text = text(&found);
    assert!(found.status.success(), "{found_text}");
    assert!(found_text.contains("Call of Duty 2 (cod2)"), "{found_text}");
    assert!(found_text.contains("Steam (app 2630)"), "{found_text}");
    assert!(found_text.contains("none missing"), "{found_text}");

    let out = tmp.path().join("copies");
    let out_arg = out.to_str().unwrap();
    let made = newcopy(
        tmp.path(),
        &["make", "cod2", "--steam", steam_arg, "--out", out_arg],
    );
    let made_text = text(&made);
    assert!(made.status.success(), "{made_text}");
    assert!(made_text.contains("Done: 30 files"), "{made_text}");
    assert!(out.join("cod2/main/iw_15.iwd").is_file());
    assert!(!out.join("cod2/CoD2MP_s.exe").exists());
    assert!(!out.join("cod2/mss32.dll").exists());

    let again = newcopy(
        tmp.path(),
        &["make", "cod2", "--steam", steam_arg, "--out", out_arg],
    );
    assert!(text(&again).contains("30 unchanged"), "{}", text(&again));

    let checked = newcopy(tmp.path(), &["check", out_arg]);
    assert!(checked.status.success(), "{}", text(&checked));
    assert!(text(&checked).contains("OK: all 30 files match"));

    // A copy that lost a file fails the check.
    fs::remove_file(out.join("cod2/main/iw_02.iwd")).unwrap();
    let checked = newcopy(tmp.path(), &["check", out_arg]);
    assert_eq!(checked.status.code(), Some(1), "{}", text(&checked));
    assert!(text(&checked).contains("main/iw_02.iwd: missing"));
}

#[test]
fn make_from_a_folder_and_refusals() {
    let tmp = TempDir::new("cli").unwrap();
    let game = tmp.path().join("CoD UO CB");
    fake_install(&game, "coduo").unwrap();
    let game_arg = game.to_str().unwrap();

    // Both games of the combined folder are listed.
    let found = newcopy(tmp.path(), &["find", "--from", game_arg]);
    let found_text = text(&found);
    assert!(found_text.contains("Call of Duty (cod)"), "{found_text}");
    assert!(found_text.contains("Call of Duty: United Offensive (coduo)"));
    assert!(found_text.contains("1.51b"));
    assert!(found_text.contains("Steam: not found"));

    // Output inside the game folder is refused, and nothing is written there.
    let inside = game.join("newcopy");
    let refused = newcopy(
        tmp.path(),
        &[
            "make",
            "coduo",
            "--from",
            game_arg,
            "--out",
            inside.to_str().unwrap(),
        ],
    );
    assert_eq!(refused.status.code(), Some(1), "{}", text(&refused));
    assert!(text(&refused).contains("inside the game folder"));
    assert!(!inside.exists());

    let out = tmp.path().join("out");
    let made = newcopy(
        tmp.path(),
        &[
            "make",
            "coduo",
            "--from",
            game_arg,
            "--out",
            out.to_str().unwrap(),
            "--copy",
        ],
    );
    assert!(made.status.success(), "{}", text(&made));
    assert!(text(&made).contains("33 copied"), "{}", text(&made));
    let manifest = fs::read_to_string(out.join("coduo/newcopy.toml")).unwrap();
    assert!(manifest.contains("source_kind = \"folder\""));
    assert!(manifest.contains("version = \"1.51b\""));
    assert!(!manifest.contains(".exe") && !manifest.contains(".dll"));

    // The wrong game for the folder.
    let wrong = newcopy(
        tmp.path(),
        &[
            "make",
            "cod2",
            "--from",
            game_arg,
            "--out",
            out.to_str().unwrap(),
        ],
    );
    assert_eq!(wrong.status.code(), Some(1));
    assert!(text(&wrong).contains("no Call of Duty 2 install recognized"));
}

#[test]
fn usage_errors() {
    let tmp = TempDir::new("cli").unwrap();
    for args in [
        &["make", "cod4", "--out", "x"][..],
        &["make", "cod2"][..],
        &["frobnicate"][..],
        &["check"][..],
        &["find", "--copy"][..],
    ] {
        let output = newcopy(tmp.path(), args);
        assert_eq!(output.status.code(), Some(2), "{args:?}: {}", text(&output));
    }
    let help = newcopy(tmp.path(), &["--help"]);
    assert!(help.status.success());
    assert!(text(&help).contains("newcopy make"));
    // No Steam in the fake HOME.
    let missing = newcopy(tmp.path(), &["make", "cod", "--out", "x"]);
    assert_eq!(missing.status.code(), Some(1));
    assert!(text(&missing).contains("wasn't found in Steam"));
}
