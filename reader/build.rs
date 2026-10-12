// Generates the trace format's types from eventtrace.proto. protox compiles the schema in Rust, so that building
// needs no protoc.
fn main() -> Result<(), Box<dyn std::error::Error>> {
    println!("cargo:rerun-if-changed=../proto/eventtrace.proto");
    let files = protox::compile(["eventtrace.proto"], ["../proto"])?;
    prost_build::Config::new().compile_fds(files)?;
    Ok(())
}
