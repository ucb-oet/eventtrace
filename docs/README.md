<!-- The documentation map: each page under docs/ and what it covers. -->

# EventTrace documentation

- [Overview](overview.md): what EventTrace records, its model of events, objects, and collections, and how a trace
  is produced.
- [Writing annotations](annotations.md): events, object types, creating and changing objects, collections (sets,
  queues, ROBs, and maps), queries, and the rules that elaboration and the runtime check.
- [Sharing collections between modules](sharing-collections.md): passing a collection to the modules that use it.
- [Annotating SystemVerilog](systemverilog.md): tracing a SystemVerilog design through DPI calls and `oet`, the C++
  library with the same model.
- [Using EventTrace in Chipyard](chipyard.md): enabling tracing, running a simulation, and recording a window of it.
  It also covers other flows.
- [Reading traces](reader.md): the `eventtrace` command, its text output, and the view files that its converters
  use.
- [Trace and manifest formats](formats.md): for writing your own tools.
