//! Every machine in `budget_cases.tsv` gets the budgets written there. The
//! same file checks the ReXGlue port's C++ (`tools/check_budgets.cpp`), so
//! both engines size themselves identically.

use machine::{budget, engine_budget, Profile};

struct Case {
    name: String,
    allowed: usize,
    quota: f64,
    cores: usize,
    own_cores: bool,
    draw_workers: usize,
    pool: usize,
    spin: bool,
    workers: usize,
    background_max: usize,
}

fn yes_no(field: &str) -> bool {
    match field {
        "yes" => true,
        "no" => false,
        other => panic!("expected yes or no, found {other:?}"),
    }
}

fn cases() -> Vec<Case> {
    include_str!("../budget_cases.tsv")
        .lines()
        .filter(|line| !line.trim().is_empty() && !line.starts_with('#'))
        .map(|line| {
            let f: Vec<&str> = line.split('\t').collect();
            assert_eq!(f.len(), 10, "{line:?}: 10 tab-separated fields");
            let n = |i: usize| {
                f[i].parse::<usize>()
                    .unwrap_or_else(|_| panic!("{line:?} field {i}"))
            };
            Case {
                name: f[0].to_string(),
                allowed: n(1),
                quota: f[2].parse().expect("quota"),
                cores: n(3),
                own_cores: yes_no(f[4]),
                draw_workers: n(5),
                pool: n(6),
                spin: yes_no(f[7]),
                workers: n(8),
                background_max: n(9),
            }
        })
        .collect()
}

#[test]
fn every_test_machine_gets_its_budget() {
    let cases = cases();
    assert!(cases.len() >= 10);
    for c in &cases {
        let profile = Profile::synthetic(c.allowed, c.cores, c.quota);
        let port = budget(&profile);
        assert_eq!(
            port.frame_threads_own_cores, c.own_cores,
            "{}: own cores",
            c.name
        );
        assert_eq!(
            port.draw_workers, c.draw_workers,
            "{}: draw workers",
            c.name
        );
        assert_eq!(port.pool_threads, c.pool, "{}: pool", c.name);
        assert_eq!(port.spinning_allowed, c.spin, "{}: spinning", c.name);
        let engine = engine_budget(&profile);
        assert_eq!(engine.workers, c.workers, "{}: engine workers", c.name);
        assert_eq!(
            engine.background_max, c.background_max,
            "{}: background",
            c.name
        );
        assert_eq!(
            engine.spinning_allowed, c.spin,
            "{}: engine spinning",
            c.name
        );
    }
}

#[test]
fn budgets_never_ask_for_more_than_the_machine_has() {
    for allowed in 1..=256 {
        for cores in [1, allowed / 2, allowed] {
            let profile = Profile::synthetic(allowed, cores.max(1), 0.0);
            let usable = profile.usable_cpus();
            let port = budget(&profile);
            let frame = if port.frame_threads_own_cores { 4 } else { 2 };
            assert!(port.pool_threads >= 1);
            // The pool gets at least one thread even on a one-CPU machine;
            // otherwise everything fits in what the machine has.
            if usable > frame + port.draw_workers {
                assert!(frame + port.draw_workers + port.pool_threads <= usable);
            }
            let engine = engine_budget(&profile);
            assert!(engine.workers >= 1 && engine.workers < usable.max(2));
            assert!(engine.background_max >= 1 && engine.background_max <= engine.workers);
        }
    }
}
