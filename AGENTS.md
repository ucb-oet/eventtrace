<!-- The rules every change to EventTrace follows, for agents and people alike. -->

# Agent instructions

Read [README.md](README.md) for the repository's layout and [docs/README.md](docs/README.md) for the documentation
map before changing the model, a frontend, the runtime, the reader, or a format. This file holds the rules that apply
to every change.

## The model and its frontends

- The Chisel frontend (`frontend/`) defines the model: events, objects, fields, collections, queries, and the rules
  elaboration and the runtime check. Add a new structure there first, such as a kind of collection, a query, a field
  property, or an event role, with its checks, documentation, and tests. Port it to `oet` (`systemverilog/`) and any
  other place only after that.
- `oet` keeps the Chisel frontend's semantics: the same names where the languages allow, and the same checks.
  [docs/systemverilog.md](docs/systemverilog.md) lists every difference; update it when one changes.
- The runtime's sources in `runtime/src` are shared by both flows. They stay C++17, as the Chisel flow compiles them;
  `oet` is C++20. A runtime change must leave the Chisel flow's behavior unchanged unless the change is meant to alter
  it.
- [docs/formats.md](docs/formats.md) is the contract for the trace and the manifest. Change it in the same commit as
  the format. `reader/proto/waterfall.proto` is a copy of illilouette's, which illilouette owns: change it there
  first, then copy it.

## Design and plans

- Plans in `docs/plans/` record only decisions agreed with the user. Keep options still under discussion in the
  conversation, even after a concession, and do not write a recommendation into a plan before it is agreed.
- When implementation reaches a rule the design leaves open, including in a test or an example, stop and raise it
  with the options. Do not pick one silently, and point out any default the implementation adopted on its own.
- Raise a design issue in four parts: what the issue is, example code where it arises, why it goes wrong, and a
  recommendation.
- Reject a rare or contrived case with the simplest check, mentioned in one line, rather than designing for it.

## Annotating designs

These apply to trace code in the designs EventTrace annotates, such as Rocket, Saturn, Sodor, and vroom.

- Sketch a change before making it: the diff of the trace code and each site, the signals each term uses, what it
  cannot tell apart, its record cost, and the checks to run. Make the diffs on scratch copies, never in the design's
  repository, and implement only after the user approves.
- Decide what to trace from the hardware's structure and what a reader of the trace needs to explain it, not from
  whether the workloads run so far happen to exercise a case. Frame a deferral as ordering.
- Trace code adds no hardware state silently. When a case seems to need a register, first look for an existing
  signal or a test on the trace's own state, then present the options and their cost to the user.
- Write an enum with one value per line, each followed by a comment saying what it means. Name a helper for what it
  finds, such as `instructionIn(stage)`, not `at` or `in`.
- Views show instructions as encoded: compressed instructions under their `c.` names, without aliases or
  pseudo-instructions.

## Verification

- Run the narrowest checks that cover the change, from this directory, after sourcing Chipyard's `env.sh`:

  | Changed | Run |
  |---|---|
  | `runtime/` | `make -C runtime test` |
  | `systemverilog/` | `make -C systemverilog test`, and `make -C examples/<example> check-sv check-sv-off` |
  | `frontend/` | `make -C examples/<example> check` for `stage0`, `stage1`, `objects`, and `collections` |
  | `reader/` | the examples' `check`, `make -C systemverilog test`, and the Python tests, which all read traces back with it |
  | `python/` | `PYTHONPATH=python EVENTTRACE_BIN=$PWD/reader/target/release/eventtrace python3 -m unittest discover -s python/tests` |

- A check kept in the repository must give independent evidence about something general. A comparison that only
  confirms one change, such as identical output before and after, is run once while making the change and not kept.
- Test supported behavior and misuses of supported features. Do not add tests that only show an unimplemented feature
  is absent.
- Keep build output, traces, and other generated files out of version control.

## Files and documentation

- Begin each new source file, other than Scala, with a comment that says what the file is for. Scala files begin
  with their package, and document their public classes and methods with Scaladoc.
- Update the documentation page that owns a behavior in the same change as the behavior. Link to it from elsewhere
  instead of repeating it.

## Git

- Make changes on a feature branch. Merge into `main` only when the user decides the feature is ready.
- Ask the user before every push, each time, even right after an earlier push was approved.
- Begin a commit subject with `EventTrace:`, or `EventTrace <component>:` for one component, such as
  `EventTrace reader:` or `EventTrace runtime:`, followed by what the change does in plain words.
