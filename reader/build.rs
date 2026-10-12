// Generates Rust types from the trace format, eventtrace.proto, and from the formats the commands write: the part of
// Perfetto's that the converter uses, and illilouette's waterfall. protox compiles the schemas in Rust, so that
// building needs no protoc.
fn main() -> Result<(), Box<dyn std::error::Error>> {
    println!("cargo:rerun-if-changed=../proto/eventtrace.proto");
    println!("cargo:rerun-if-changed=proto/perfetto.proto");
    println!("cargo:rerun-if-changed=proto/waterfall.proto");
    let files = protox::compile(["eventtrace.proto", "perfetto.proto", "waterfall.proto"], ["../proto", "proto"])?;
    prost_build::Config::new().compile_fds(files)?;
    Ok(())
}
