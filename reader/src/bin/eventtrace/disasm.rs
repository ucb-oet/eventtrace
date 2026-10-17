//! RISC-V instructions as text, through rvdasm, as GNU objdump prints them with `-M no-aliases`, for a core with an
//! ISA string such as rv32i or rv64gc: each instruction as it is encoded, a compressed one under its own name, and no
//! pseudo-instructions.

use std::collections::HashMap;

use eventtrace::{Error, Result};
use rvdasm::disassembler::{self, Xlen};

pub struct Disassembler {
    rvdasm: disassembler::Disassembler,
    compressed: bool,
    cache: HashMap<(u64, u64), String>,
}

impl Disassembler {
    pub fn new(isa: &str) -> Result<Self> {
        let isa = isa.to_lowercase();
        let xlen = match isa.get(..4) {
            Some("rv32") => Xlen::XLEN32,
            Some("rv64") => Xlen::XLEN64,
            _ => return Err(Error(format!("ISA string {isa} does not start with rv32 or rv64"))),
        };
        // Only the C extension changes how instructions decode, since rvdasm knows every other extension at once.
        let compressed = isa[4..].split('_').next().unwrap_or("").contains('c');
        Ok(Disassembler { rvdasm: disassembler::Disassembler::new(xlen), compressed, cache: HashMap::new() })
    }

    /// The instruction `bits` at `pc` as text, with a PC-relative target as its absolute address, or "unknown 0x..."
    /// if it does not decode.
    pub fn text(&mut self, bits: u64, pc: u64) -> &str {
        if !self.cache.contains_key(&(bits, pc)) {
            let text = self.disassemble(bits, pc);
            self.cache.insert((bits, pc), text);
        }
        &self.cache[&(bits, pc)]
    }

    fn disassemble(&self, bits: u64, pc: u64) -> String {
        // With the C extension, an instruction whose low two bits are not 11 is compressed, in the low half; without
        // it, there is no such instruction.
        let size = if bits & 3 == 3 { 4 } else { 2 };
        let unknown = || format!("unknown 0x{bits:0width$x}", width = 2 * size);
        if size == 2 && !self.compressed {
            return unknown();
        }
        let word = if size == 2 { bits & 0xffff } else { bits & 0xffff_ffff };
        self.rvdasm.disassmeble_one(word as u32).map_or_else(unknown, |insn| insn.asm_no_aliases(pc))
    }
}
