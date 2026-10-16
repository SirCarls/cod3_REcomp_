//! Prints what this machine has and the thread budgets it gets:
//! `cargo run -p machine --example show`.

fn main() {
    let profile = machine::get();
    println!("{}", profile.summary());
    println!();
    print!("{}", profile.describe());
    println!();
    println!("ReXGlue port: {}", machine::budget(profile).describe());
    println!(
        "Rust engine:  {}",
        machine::engine_budget(profile).describe()
    );
}
