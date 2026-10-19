# EventTrace documentation

- [Overview](overview.md): what EventTrace records, its model of events, objects, and collections, and how a trace
  is produced.
- [Writing annotations](annotations.md): events, object types, creating and changing objects, collections, queries,
  and the rules that elaboration and the runtime check.
- [Sharing collections between modules](sharing-collections.md): passing a collection to the modules that use it.
- [Using EventTrace in Chipyard](chipyard.md): enabling tracing, running a simulation, and recording a window of it.
  It also covers other flows.
- [Reading traces](reader.md): the `eventtrace` command, its text output, and the view files that its converters
  use.
- [Trace and manifest formats](formats.md): for writing your own tools.
