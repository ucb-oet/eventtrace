from __future__ import annotations

import gzip
import hashlib
import json
import os
import re
import shutil
import subprocess
import tempfile
import unittest
from enum import IntEnum
from pathlib import Path
from typing import Annotated

from eventtrace import (
    I64,
    U32,
    U64,
    Const,
    Counter,
    Id,
    Identity,
    Lifecycle,
    Mutable,
    Role,
    TracedObject,
    TraceSource,
    Writer,
    event,
    object_type,
)


class Location(IntEnum):
    Fetch = 0
    S1 = 1
    Execute = 2


@object_type(name="instruction")
class Instruction(TracedObject):
    id: Identity[U64]
    pc: Const[U32]
    location: Mutable[Location]

    def __init__(self, id, pc, writer=None, scope="core"):
        self.id, self.pc = id, pc
        self.bind_trace(writer, scope=scope)
        self.extractions = 0

    @property
    def bits(self) -> Const[U32]:
        self.extractions += 1
        return 0x13

    @event(lifecycle=Lifecycle.CREATE, location=Location.Fetch)
    def fetch(self, cycle: int): ...

    @event(location=Location.S1)
    def enter_s1(self, cycle: int): ...

    @event
    def issue(self, cycle: int, location: Location = Location.Execute): ...

    @event(blocks=True)
    def blocked(
        self, cycle: int, by: Annotated[Id[Instruction] | None, Role.BLOCKED_BY] = None
    ): ...

    @event
    def sample(self, cycle: int, value: I64, flag: bool): ...

    @event(lifecycle=Lifecycle.COMPLETE)
    def complete(self, cycle: int): ...

    @event(lifecycle=Lifecycle.ABORT)
    def abort(self, cycle: int): ...


class Registers(TraceSource):
    @event
    def write(self, cycle: int, index: U32, value: Counter[U64]): ...


class SDKTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.binary = os.environ.get("EVENTTRACE_BIN")

    def tearDown(self):
        self.temp.cleanup()

    def writer(self, name="run.eventtrace", **kwargs):
        return Writer(
            self.root / name,
            sources=[(Instruction, "core"), (Registers, "core")],
            **kwargs,
        )

    def fixture(self, name="run.eventtrace"):
        writer = self.writer(
            name,
            instruction_text={
                "instruction": {"field": "bits", "values": {"19": "addi x0, x0, 0"}}
            },
        )
        a, b = Instruction(42, 0x100, writer), Instruction(900, 0x104, writer)
        a.fetch(1)
        b.fetch(1)  # same site and cycle, distinct caller-owned IDs
        a.enter_s1(2)
        a.issue(2)  # last transition defines the next cycle's interval
        b.blocked(2, by=a)
        a.sample(2, -(1 << 63), True)
        b.sample(2, (1 << 63) - 1, False)
        b.blocked(3)  # optional reference absent
        Registers().bind_trace(writer, scope="core").write(3, 7, 123)
        a.complete(4)
        b.abort(4)
        writer.close()
        self.assertEqual(a.extractions, 1)
        self.assertEqual(b.extractions, 1)
        return writer

    def test_manifest_includes_unexecuted_methods_and_exact_hash(self):
        writer = self.fixture()
        manifest = json.loads(writer.manifest_path.read_bytes())
        self.assertEqual(len(manifest["events"]), 8)
        self.assertEqual(manifest["objects"][0]["name"], "instruction")
        blocked = next(
            e for e in manifest["events"] if e["name"] == "instruction.blocked"
        )
        self.assertEqual(blocked["fields"][0]["role"], "blocked_by")
        self.assertTrue(blocked["fields"][0]["optional"])
        site = next(
            s for s in manifest["sites"] if s["event_name"] == "instruction.blocked"
        )
        self.assertEqual(
            set(site), {"site_id", "event_name", "instance", "source", "blocks"}
        )
        self.assertEqual(manifest["counters"][0]["value"], "value")
        digest = hashlib.sha256(writer.manifest_path.read_bytes()).hexdigest().encode()
        self.assertIn(digest, writer.path.read_bytes())

    def test_disabled_methods_do_not_extract_fields(self):
        a = Instruction(42, 0x100)
        a.fetch(1)
        a.issue(2)
        self.assertEqual(a.extractions, 0)

    def test_invalid_calls_do_not_advance_writer(self):
        with self.writer() as writer:
            a = Instruction(1, 0x100, writer)
            a.fetch(1)
            with self.assertRaises(TypeError):
                a.issue(99, location=2)
            with self.assertRaises(ValueError):
                a.sample(99, 1 << 63, False)
            with self.assertRaises(TypeError):
                a.sample(99, 0, 1)
            with self.assertRaises(ValueError):
                a.issue(-1)
            a.issue(2)
            with self.assertRaises(ValueError):
                a.issue(1)
        with self.assertRaises(ValueError):
            a.complete(3)
        writer.close()

    def test_scope_and_id_validation(self):
        with self.writer() as writer:
            with self.assertRaises(ValueError):
                Instruction(-1, 4, writer).fetch(1)
            with self.assertRaises(ValueError):
                Instruction(1, 1 << 32, writer).fetch(1)
            with self.assertRaises(ValueError):
                Instruction(1, 4, writer, scope="other").fetch(1)

    def test_gzip_has_identical_payload(self):
        plain = self.fixture()
        compressed = self.fixture("run.eventtrace.gz")
        self.assertEqual(
            gzip.decompress(compressed.path.read_bytes()), plain.path.read_bytes()
        )

    def test_waterfall_stalls_are_independent_of_stage_duration(self):
        if not self.binary or not shutil.which("protoc"):
            self.skipTest("set EVENTTRACE_BIN and install protoc for waterfall checks")
        writer = self.writer()
        a, b, c, fast = [Instruction(i, i * 4, writer) for i in range(1, 5)]
        for obj in (a, b, c, fast):
            obj.fetch(1)
        a.issue(2)
        c.issue(2)
        fast.enter_s1(2)
        fast.complete(2)  # its next stage would start after its life ends
        b.blocked(2)
        a.blocked(4)
        c.blocked(4)
        b.abort(4)
        a.blocked(5)  # repeated observations coalesce into the same interval
        a.issue(6)  # assigning Execute again must not clear the stall
        a.enter_s1(8)  # actual stage change clears it at 9
        a.issue(10)
        a.complete(20)  # long execution has no inferred blocking
        writer.close()
        view = self.root / "view.json"
        view.write_text(
            json.dumps(
                {
                    "object": "instruction",
                    "tracks": "location",
                    "label": "pc",
                    "created_in": {"fetch": "Fetch"},
                }
            )
        )
        output = self.root / "stalls.waterfall"
        subprocess.run(
            [
                self.binary,
                "waterfall",
                str(view),
                str(writer.manifest_path),
                str(writer.path),
                str(output),
            ],
            check=True,
        )
        reader = Path(__file__).resolve().parents[2] / "reader"
        decoded = subprocess.check_output(
            [
                "protoc",
                "--decode=illilouette.Waterfall",
                f"--proto_path={reader / 'proto'}",
                "waterfall.proto",
            ],
            input=output.read_bytes(),
        ).decode()
        self.assertTrue(decoded.startswith("version: 1\n"))
        self.assertNotIn("blocked {", decoded)
        rows = re.findall(r"^rows \{\n(.*?)^\}", decoded, re.MULTILINE | re.DOTALL)
        segments = []
        for row in rows:
            display = []
            for body in re.findall(
                r"^  segments \{\n(.*?)^  \}", row, re.MULTILINE | re.DOTALL
            ):
                fields = dict(re.findall(r"(from|stage|held): (\d+)", body))
                display.append(
                    tuple(int(fields.get(f, 0)) for f in ("from", "stage", "held"))
                )
            segments.append(display)
        self.assertEqual(
            segments,
            [
                [(1, 0, 0), (3, 2, 0), (4, 2, 4), (9, 1, 0), (11, 2, 0)],
                [(1, 0, 0), (2, 0, 2)],
                [(1, 0, 0), (3, 2, 0), (4, 2, 17)],
                [(1, 0, 0)],
            ],
        )

    def test_rust_reader_and_exporters(self):
        if not self.binary:
            self.skipTest("set EVENTTRACE_BIN to run reader interoperability checks")
        for name in ("run.eventtrace", "run.eventtrace.gz"):
            writer = self.fixture(name)
            dump = subprocess.check_output(
                [self.binary, "dump", str(writer.manifest_path), str(writer.path)],
                text=True,
            )
            self.assertIn("subject=instruction#42", dump)
            self.assertIn("subject=instruction#900", dump)
            self.assertIn("by=instruction#42", dump)
            self.assertIn("value=-9223372036854775808", dump)
            self.assertIn("value=9223372036854775807", dump)
            self.assertIn("flag=0", dump)
            view = self.root / "view.json"
            view.write_text(
                json.dumps(
                    {
                        "object": "instruction",
                        "tracks": "location",
                        "label": "pc",
                        "created_in": {"fetch": "Fetch"},
                    }
                )
            )
            for command, proto, message in (
                ("perfetto", "perfetto", "perfetto.protos.Trace"),
                ("waterfall", "waterfall", "illilouette.Waterfall"),
            ):
                out = self.root / command
                subprocess.run(
                    [
                        self.binary,
                        command,
                        str(view),
                        str(writer.manifest_path),
                        str(writer.path),
                        str(out),
                    ],
                    check=True,
                )
                self.assertGreater(out.stat().st_size, 0)
                if shutil.which("protoc"):
                    reader = Path(__file__).resolve().parents[2] / "reader"
                    text = subprocess.check_output(
                        [
                            "protoc",
                            f"--decode={message}",
                            f"--proto_path={reader / 'proto'}",
                            f"{proto}.proto",
                        ],
                        input=out.read_bytes(),
                    ).decode()
                    self.assertIn("addi x0, x0, 0", text)
                    if command == "perfetto":
                        self.assertIn("counter_value: 123", text)
                        self.assertIn("terminating_flow_ids", text)
                        # Coalescing S1+Execute must not leak an unterminated slice.
                        self.assertEqual(
                            text.count("type: TYPE_SLICE_BEGIN"),
                            text.count("type: TYPE_SLICE_END"),
                        )
                        self.assertNotIn('name: "S1"', text)


if __name__ == "__main__":
    unittest.main()
