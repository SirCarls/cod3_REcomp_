// [acct2-decomp] Links the SDK's shader interpreter (interp/build.sh's libxe_interp.a) and the
// SDK runtime it needs for guest memory (librexruntime.so).
fn main() {
    let interp = std::env::var("XE_INTERP_DIR").expect("XE_INTERP_DIR: the folder interp/build.sh wrote libxe_interp.a to");
    let runtime = std::env::var("XE_RUNTIME_LIB").expect("XE_RUNTIME_LIB: the folder with the SDK's librexruntime.so");
    println!("cargo:rustc-link-search=native={}", interp);
    println!("cargo:rustc-link-lib=static=xe_interp");
    println!("cargo:rustc-link-search=native={}", runtime);
    println!("cargo:rustc-link-lib=dylib=rexruntime");
    println!("cargo:rustc-link-lib=dylib=stdc++");
    println!("cargo:rustc-link-arg=-Wl,-rpath,{}", runtime);
    println!("cargo:rerun-if-env-changed=XE_INTERP_DIR");
    println!("cargo:rerun-if-env-changed=XE_RUNTIME_LIB");
    println!("cargo:rerun-if-changed={}/libxe_interp.a", interp);
}
