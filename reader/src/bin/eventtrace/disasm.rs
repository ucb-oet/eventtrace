//! RISC-V instructions as text, through capstone, for a core with an ISA string such as rv32i or rv64gc.

use std::collections::HashMap;

use capstone::arch::riscv::{ArchExtraMode, ArchMode, RiscVOperand};
use capstone::arch::{ArchOperand, BuildsCapstone, BuildsCapstoneExtraMode};
use capstone::Capstone;
use eventtrace::{Error, Result};

/// Instructions whose last operand is an offset from their own PC, which capstone prints as the offset.
const PC_RELATIVE: &[&str] = &[
    "j", "jal", "c.j", "c.jal", "beq", "bne", "blt", "bge", "bltu", "bgeu", "beqz", "bnez", "blez", "bgez", "bltz",
    "bgtz", "bgt", "ble", "bgtu", "bleu", "c.beqz", "c.bnez",
];

pub struct Disassembler {
    capstone: Capstone,
    compressed: bool,
    mask: u64,
    cache: HashMap<(u64, u64), String>,
}

impl Disassembler {
    pub fn new(isa: &str) -> Result<Self> {
        let isa = isa.to_lowercase();
        let (mode, xlen) = match isa.get(..4) {
            Some("rv32") => (ArchMode::RiscV32, 32),
            Some("rv64") => (ArchMode::RiscV64, 64),
            _ => return Err(Error(format!("ISA string {isa} does not start with rv32 or rv64"))),
        };
        // Only the C extension changes how instructions decode, since capstone knows every other extension at once.
        let compressed = isa[4..].split('_').next().unwrap_or("").contains('c');
        let extra = if compressed { vec![ArchExtraMode::RiscVC] } else { vec![] };
        let capstone = Capstone::new()
            .riscv()
            .mode(mode)
            .extra_mode(extra.into_iter())
            .detail(true)
            .build()
            .map_err(|e| Error(format!("capstone: {e}")))?;
        Ok(Disassembler { capstone, compressed, mask: u64::MAX >> (64 - xlen), cache: HashMap::new() })
    }

    /// The instruction `bits` at `pc` as text, with a PC-relative target made absolute, or "unknown 0x..." if it
    /// does not decode.
    pub fn text(&mut self, bits: u64, pc: u64) -> &str {
        if !self.cache.contains_key(&(bits, pc)) {
            let text = self.disassemble(bits, pc);
            self.cache.insert((bits, pc), text);
        }
        &self.cache[&(bits, pc)]
    }

    fn disassemble(&self, bits: u64, pc: u64) -> String {
        // With the C extension, an instruction whose low two bits are not 11 is compressed, in the low half.
        let size = if bits & 3 == 3 || !self.compressed { 4 } else { 2 };
        let unknown = || format!("unknown 0x{bits:0width$x}", width = 2 * size);
        let Ok(insns) = self.capstone.disasm_count(&bits.to_le_bytes()[..size], pc, 1) else { return unknown() };
        let Some(insn) = insns.iter().next() else { return unknown() };
        let mnemonic = insn.mnemonic().unwrap_or("");
        let operands = insn.op_str().unwrap_or("");
        if PC_RELATIVE.contains(&mnemonic) {
            let offset = self.capstone.insn_detail(insn).ok().and_then(|d| match d.arch_detail().operands().last() {
                Some(ArchOperand::RiscVOperand(RiscVOperand::Imm(offset))) => Some(*offset),
                _ => None,
            });
            if let Some(offset) = offset {
                let target = pc.wrapping_add(offset as u64) & self.mask;
                return match operands.rsplit_once(',') {
                    Some((registers, _)) => format!("{mnemonic} {registers}, 0x{target:x}"),
                    None => format!("{mnemonic} 0x{target:x}"),
                };
            }
        }
        format!("{mnemonic} {operands}").trim_end().to_string()
    }
}
