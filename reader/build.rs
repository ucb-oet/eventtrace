// Generates Rust types from the trace format, eventtrace.proto, and from the part of Perfetto's format that the
// converter writes. protox compiles the schemas in Rust, so that building needs no protoc.
fn main() -> Result<(), Box<dyn std::error::Error>> {
    println!("cargo:rerun-if-changed=../proto/eventtrace.proto");
    println!("cargo:rerun-if-changed=proto/perfetto.proto");
    let files = protox::compile(["eventtrace.proto", "perfetto.proto"], ["../proto", "proto"])?;
    prost_build::Config::new().compile_fds(files)?;
    Ok(())
}
