// SPDX-License-Identifier: MIT
fn main() {
    println!("cargo::rustc-check-cfg=cfg(crossink_rar_helper)");
    println!("cargo::rustc-cfg=crossink_rar_helper");
    println!("cargo::rerun-if-changed=../src/input.rs");
}
