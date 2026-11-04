<!-- The Python SDK's package page: what it records, how to install it, and where its API is documented. -->

# EventTrace Python SDK

Record events from Python models using EventTrace's raw protobuf format and
JSON manifest. Models supply their own global cycles and object IDs. The writer
buffers only the current cycle and has no runtime dependencies.

Requires Python 3.11 or newer. From an EventTrace checkout:

```sh
python3 -m pip install ./python
```

The package does not require Chipyard, Chisel, or the Rust reader. Declare event
schemas on model classes with `@object_type` and `@event`, then bind model objects
to a `Writer`. Event method bodies are declarations replaced by recorders, so
they must not contain simulation behavior. Calls such as `instruction.fetch(cycle)`
record the model's supplied time and identity. A `.gz` output suffix enables gzip.

See the [SDK usage and examples](https://github.com/ucb-oet/eventtrace#native-python-producers)
and [trace format](https://github.com/ucb-oet/eventtrace/blob/main/docs/formats.md)
for the declaration API, lifecycle rules, and manifest contract.

The separately built EventTrace reader converts recordings to Perfetto and
waterfall views. The SDK and reader live in the same repository so their format
compatibility can be tested together.
