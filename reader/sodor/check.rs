//! Checks EventTrace's records of Sodor's five-stage pipeline against Sodor itself.
//!
//! Stage 5 tooling, which may not stay. Run riscv-tests on TracedSodor5StageConfig, writing each test's records next to
//! its printf log, and optionally on the plain Sodor5StageConfig too:
//!
//!   out=$PWD/sims/verilator/output/chipyard.harness.TestHarness.TracedSodor5StageConfig
//!   make -C sims/verilator CONFIG=TracedSodor5StageConfig run-binary BINARY=$test \
//!     EXTRA_SIM_FLAGS=+eventtrace=$out/$(basename $test).eventtrace
//!   make -C sims/verilator CONFIG=Sodor5StageConfig run-binary BINARY=$test
//!
//! then
//!
//!   tools/eventtrace/reader/target/release/check_sodor <traced output dir> --manifest <manifest>
//!     [--plain <plain output dir>] <test>...
//!
//! The manifest is Chipyard's sims/verilator/generated-src/<long name>/<long name>.eventtrace.json. For each test it
//! reads <test>.out, Sodor's per-cycle printf log, and <test>.eventtrace, and checks:
//!  1. Function: with --plain, the printf log is identical without tracing, so tracing changes nothing Sodor does.
//!  2. Retirement: EventTrace retires the instructions Sodor's printf reports retiring, in order, with the PC and bits
//!     of their decode records, a constant number of cycles apart.
//!  3. Lifetimes: replaying the records, each event acts on an instruction in the stage it names, no instruction
//!     changes twice in a cycle, each stall's blocker and each squash's branch are in Execute, and no stage holds two
//!     instructions.
//!  4. Bandwidth: records and trace bytes per cycle, over the cycles Sodor's printf covers.
//!
//! The test's own outcome is reported separately: a test that fails both with and without tracing is no tracing
//! problem.

use std::collections::{BTreeMap, BTreeSet, HashMap};
use std::path::PathBuf;
use std::process::ExitCode;

use clap::Parser;
use eventtrace::objects::Cycles;
use eventtrace::{Error, Events, Manifest, Result, Value};

#[derive(Parser)]
#[command(name = "check_sodor", about = "Checks EventTrace's records of Sodor's five-stage pipeline against Sodor")]
struct Args {
    /// Output directory of the TracedSodor5StageConfig runs
    traced: PathBuf,
    /// The design's manifest, <long name>.eventtrace.json
    #[arg(long)]
    manifest: PathBuf,
    /// Output directory of the Sodor5StageConfig runs
    #[arg(long)]
    plain: Option<PathBuf>,
    #[arg(required = true)]
    tests: Vec<String>,
}

/// The stages each event takes its subject from, and the stage it moves it to; None destroys it.
fn moves(event: &str) -> Option<(&'static [&'static str], Option<&'static str>)> {
    Some(match event {
        "instruction.issue" => (&["Decode"], Some("Execute")),
        "instruction.memory" => (&["Execute"], Some("Memory")),
        "instruction.writeback" => (&["Memory"], Some("Writeback")),
        "instruction.retire" => (&["Writeback"], None),
        "instruction.squash" => (&["Decode"], None),
        "instruction.flush" => (&["Decode", "Execute"], None),
        "instruction.trap" => (&["Memory"], None),
        _ => return None,
    })
}

/// The stage an event that leaves its subject where it is finds it in.
fn stays(event: &str) -> Option<&'static str> {
    match event {
        "instruction.stall" => Some("Decode"),
        "instruction.mem_wait" => Some("Memory"),
        _ => None,
    }
}

/// A retirement Sodor's printf reports, from a line such as
///   Cyc=         42 [1] pc=[80000000] W[r 0=00000000][1] ... inst=[0500006f]     j       pc + 0x50
/// as (cycle, pc, bits); the bracketed digit after the cycle is 1 when an instruction retires.
fn sodor_retirement(line: &str) -> Option<(u64, u64, u64)> {
    let rest = line.strip_prefix("Cyc=")?.trim_start();
    let (cycle, rest) = rest.split_once(' ')?;
    let rest = rest.strip_prefix("[1] pc=[")?;
    let (pc, rest) = rest.split_once(']')?;
    let (_, inst) = rest.rsplit_once("inst=[")?;
    let (inst, _) = inst.split_once(']')?;
    let number = |s: &str, radix| u64::from_str_radix(s, radix).ok();
    Some((number(cycle, 10)?, number(pc, 16)?, number(inst, 16)?))
}

/// What a test's records amount to.
#[derive(Default)]
struct Replay {
    /// Each retirement, as (cycle, pc, bits).
    retired: Vec<(u64, u64, u64)>,
    counts: BTreeMap<String, u64>,
    records: u64,
    peak: u64,
}

/// Replays the records cycle by cycle, checking each event against the stages of the instructions before the edge.
fn replay(manifest: &Manifest, events: Events<std::fs::File>, problems: &mut Vec<String>) -> Result<Replay> {
    let mut replay = Replay::default();
    let mut live: HashMap<u64, &'static str> = HashMap::new();
    let mut decoded: HashMap<u64, (u64, u64)> = HashMap::new();
    let mut next_id = 0;
    let unsigned = |e: &eventtrace::Event, field: &str| e.get(manifest, field).and_then(Value::unsigned);
    for cycle in Cycles::new(events) {
        let cycle = cycle?;
        let c = cycle.cycle;
        replay.records += cycle.events.len() as u64;
        replay.peak = replay.peak.max(cycle.events.len() as u64);
        // Every event of a cycle sees the state before its clock edge.
        let mut stages: BTreeMap<&str, u32> = BTreeMap::new();
        for stage in live.values() {
            *stages.entry(stage).or_default() += 1;
        }
        if stages.values().any(|&n| n > 1) {
            problems.push(format!("cycle {c}: a stage holds two instructions: {stages:?}"));
        }
        let mut changes: HashMap<u64, Option<&'static str>> = HashMap::new();
        for e in &cycle.events {
            let name = manifest.events[e.event].name.as_str();
            *replay.counts.entry(name.to_string()).or_default() += 1;
            let Some(i) = e.subject else {
                problems.push(format!("cycle {c}: unknown event {name}"));
                continue;
            };
            let at = |id: u64| live.get(&id).copied().unwrap_or("None");
            if name == "instruction.decode" {
                if i != next_id {
                    problems.push(format!("cycle {c}: created instruction#{i}, expected #{next_id}"));
                }
                next_id = i + 1;
                changes.insert(i, Some("Decode"));
                decoded.insert(i, (unsigned(e, "pc").unwrap_or(0), unsigned(e, "bits").unwrap_or(0)));
            } else if let Some((sources, target)) = moves(name) {
                if !sources.contains(&at(i)) {
                    problems.push(format!("cycle {c}: {name} on instruction#{i}, which is in {}", at(i)));
                }
                if changes.insert(i, target).is_some() {
                    problems.push(format!("cycle {c}: instruction#{i} changes twice"));
                }
                if name == "instruction.retire" {
                    let (pc, bits) = decoded.get(&i).copied().unwrap_or_default();
                    replay.retired.push((c, pc, bits));
                }
                if name == "instruction.squash" {
                    let by = unsigned(e, "by").unwrap_or(u64::MAX);
                    if at(by) != "Execute" {
                        problems.push(format!("cycle {c}: squash by instruction#{by}, which is not in Execute"));
                    }
                }
            } else if let Some(stage) = stays(name) {
                if at(i) != stage {
                    problems.push(format!("cycle {c}: {name} on instruction#{i}, which is in {}", at(i)));
                }
                if name == "instruction.stall" {
                    let blocker = unsigned(e, "blocker").unwrap_or(u64::MAX);
                    if at(blocker) != "Execute" {
                        problems.push(format!("cycle {c}: stall blocked by instruction#{blocker}, which is not in Execute"));
                    }
                }
            } else {
                problems.push(format!("cycle {c}: unknown event {name}"));
            }
        }
        for (i, target) in changes {
            match target {
                Some(stage) => live.insert(i, stage),
                None => live.remove(&i),
            };
        }
    }
    Ok(replay)
}

#[derive(Default)]
struct Totals {
    cycles: u64,
    records: u64,
    bytes: u64,
    peak: u64,
    retired: u64,
    events: BTreeMap<String, u64>,
}

/// Checks one test, printing its line; whether tracing matches Sodor.
fn check(test: &str, args: &Args, manifest: &Manifest, totals: &mut Totals) -> Result<bool> {
    let read = |path: PathBuf| std::fs::read_to_string(&path).map_err(|e| Error(format!("cannot read {}: {e}", path.display())));
    let mut problems = Vec::new();
    let out = read(args.traced.join(format!("{test}.out")))?;
    let passed = out.contains("*** PASSED ***");
    if let Some(plain) = &args.plain {
        if read(plain.join(format!("{test}.out")))? != out {
            problems.push("the printf log differs without tracing".to_string());
        }
    }
    let sodor: Vec<(u64, u64, u64)> = out.lines().filter_map(sodor_retirement).collect();
    let cycles = out.lines().filter(|line| line.starts_with("Cyc=")).count() as u64;

    let trace = args.traced.join(format!("{test}.eventtrace"));
    let ours = replay(manifest, Events::open(manifest, &trace)?, &mut problems)?;
    if ours.retired.len() != sodor.len() {
        problems.push(format!("EventTrace retires {} instructions, Sodor {}", ours.retired.len(), sodor.len()));
    }
    let pairs = || ours.retired.iter().zip(&sodor);
    if let Some((o, s)) = pairs().find(|(o, s)| (o.1, o.2) != (s.1, s.2)) {
        problems.push(format!("first retirement mismatch: EventTrace (cycle, pc, bits) {o:?}, Sodor {s:?}"));
    }
    let offsets: BTreeSet<i64> = pairs().map(|(o, s)| o.0 as i64 - s.0 as i64).collect();
    if offsets.len() > 1 {
        problems.push(format!("retirements are not a constant number of cycles apart: {:?}", offsets.iter().take(5).collect::<Vec<_>>()));
    }

    totals.cycles += cycles;
    totals.records += ours.records;
    totals.bytes += std::fs::metadata(&trace).map(|m| m.len()).unwrap_or(0);
    totals.peak = totals.peak.max(ours.peak);
    totals.retired += ours.retired.len() as u64;
    for (name, n) in &ours.counts {
        *totals.events.entry(name.clone()).or_default() += n;
    }
    println!(
        "{test:28} {:>4} {:>8} {cycles:>8} {:>8.3}  {}",
        if passed { "pass" } else { "FAIL" },
        ours.retired.len(),
        ours.records as f64 / cycles.max(1) as f64,
        if problems.is_empty() { "ok" } else { "MISMATCH" }
    );
    for problem in problems.iter().take(5) {
        println!("    {problem}");
    }
    Ok(problems.is_empty())
}

fn run(args: &Args) -> Result<bool> {
    let manifest = Manifest::load(&args.manifest)?;
    let mut totals = Totals::default();
    println!("{:28} {:>4} {:>8} {:>8} {:>8}  tracing", "test", "test", "retired", "cycles", "rec/cyc");
    let mut ok = 0;
    for test in &args.tests {
        ok += check(test, args, &manifest, &mut totals)? as usize;
    }
    println!("\ntracing matches Sodor in {ok} of {} tests; {} retirements compared", args.tests.len(), totals.retired);
    let events: Vec<String> = totals
        .events
        .iter()
        .map(|(name, n)| format!("{} {n}", name.rsplit('.').next().unwrap_or(name)))
        .collect();
    println!("events: {}", events.join(", "));
    let cycles = totals.cycles.max(1) as f64;
    let retired = totals.retired.max(1) as f64;
    println!(
        "bandwidth: {:.3} records per cycle, at most {} in one cycle; {:.2} per retired instruction; \
         {:.2} trace bytes per cycle, {:.1} per retired instruction",
        totals.records as f64 / cycles,
        totals.peak,
        totals.records as f64 / retired,
        totals.bytes as f64 / cycles,
        totals.bytes as f64 / retired,
    );
    Ok(ok == args.tests.len())
}

fn main() -> ExitCode {
    match run(&Args::parse()) {
        Ok(true) => ExitCode::SUCCESS,
        Ok(false) => ExitCode::FAILURE,
        Err(e) => {
            eprintln!("check_sodor: {e}");
            ExitCode::from(2)
        }
    }
}
