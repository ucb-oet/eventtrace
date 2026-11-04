# Structures check: the records match a cycle-by-cycle model of the pipeline exactly, and the run exercises what
# each kind of collection is for.
# Run as: awk -f check.awk eventtrace_manifest.json run.log
#
# TestTop holds reset for edges 0 to 2, so records come from edges 3 to 82; the model's step k is edge k + 3. Each step
# reads the registers from before the edge, as the trace does, then updates them as the RTL does:
#   execute: the lowest reservation-station slot whose wait is 0; a PC of 5 mod 8 is a branch that mispredicts.
#   commit: the ROB's head, once done.
#   dispatch: the fetch queue's head into the ROB's tail and the lowest free slot, unless the queue is empty, the ROB
#     is full, every slot is busy, or a branch mispredicts. The slot waits ((PC mod 8) xor 5) or (PC bit 3, shifted
#     to bit 2) cycles: a branch at once for 5 mod 16 and after 4 cycles for 13 mod 16.
#   jump: a dispatched PC of 14 mod 16, after which the fetch queue's younger entries leave and fetch restarts at
#     the jump's PC + 3.
#   fetch: the next PC, unless the fetch queue is full, a branch mispredicts, or a jump dispatches.
#   mispredict: the ROB's tail moves back to just after the branch, younger operations leave the reservation station,
#     the fetch queue empties, and fetch restarts at the branch's PC + 1.
# Records are compared as "edge event PC [mispredict]", naming each object by its PC, since squashed PCs are fetched
# again. Per edge, the model's lines and the trace's are sorted, so their order within an edge is not compared; the
# runtime orders records by site.
function fail(message) {
  print "FAIL: " message
  bad = 1
}

function emit(line) { want[++nwant] = line }

# The exclusive or of two 3-bit values.
function xor3(a, b,    i, r) {
  r = 0
  for (i = 0; i < 3; i++) if (int(a / 2 ^ i) % 2 != int(b / 2 ^ i) % 2) r += 2 ^ i
  return r
}

function model(steps,    k, s, e, c, exe, slot, pc, tag, misp, head, commit, count, free, disp, dpc, jump, fetch, cut,
               n, i) {
  next_pc = 0; fq_head = 0; fq_count = 0; rob_head = 0; rob_tail = 0
  for (e = 0; e < 8; e++) rob_valid[e] = 0
  for (s = 0; s < 4; s++) rs_valid[s] = 0
  for (k = 0; k < steps; k++) {
    c = k + 3
    exe = 0
    for (s = 0; s < 4 && !exe; s++) if (rs_valid[s] && rs_wait[s] == 0) { exe = 1; slot = s }
    if (exe) { pc = rs_pc[slot]; tag = rs_tag[slot] }
    misp = exe && pc % 8 == 5
    head = rob_head % 8
    commit = rob_valid[head] && rob_done[head]
    count = (rob_tail - rob_head + 16) % 16
    free = -1
    for (s = 3; s >= 0; s--) if (!rs_valid[s]) free = s
    disp = fq_count > 0 && count != 8 && free >= 0 && !misp
    dpc = fq_pc[fq_head]
    jump = disp && dpc % 16 == 14
    fetch = fq_count != 4 && !misp && !jump

    if (fetch) emit(c " insn.fetch " next_pc)
    if (disp) { emit(c " insn.dispatch " dpc); emit(c " uop.dispatch " dpc) }
    if (jump) {
      for (i = 1; i < fq_count; i++) emit(c " insn.flush " fq_pc[(fq_head + i) % 4])
      if (fq_count > 1) jump_drops++; else jumps_alone++
    }
    if (exe) { emit(c " uop.execute " pc); emit(c " insn.complete " pc " " misp) }
    if (misp) {
      cut = (tag + 1) % 16
      n = (rob_tail - cut + 16) % 16
      for (i = 0; i < n; i++) emit(c " insn.squash " rob_pc[(cut + i) % 8])
      for (s = 0; s < 4; s++) if (rs_valid[s] && rs_pc[s] > pc) emit(c " uop.squash " rs_pc[s])
      for (i = 0; i < fq_count; i++) emit(c " insn.flush " fq_pc[(fq_head + i) % 4])
      if (n > 0) rollbacks++; else empty_rollbacks++
      if (fq_count > 0) flushes++
    }
    if (commit) emit(c " insn.commit " rob_pc[head])
    if (fq_count == 4) fq_full++
    if (free < 0) rs_full++

    for (s = 0; s < 4; s++) if (rs_valid[s] && rs_wait[s] > 0) rs_wait[s]--
    if (exe) { rs_valid[slot] = 0; rob_done[tag % 8] = 1 }
    if (commit) { rob_valid[head] = 0; rob_head = (rob_head + 1) % 16 }
    if (misp) {
      for (i = 0; i < n; i++) rob_valid[(cut + i) % 8] = 0
      rob_tail = cut
      for (s = 0; s < 4; s++) if (rs_pc[s] > pc) rs_valid[s] = 0
      fq_count = 0
      next_pc = pc + 1
    } else {
      if (disp) {
        e = rob_tail % 8
        rob_valid[e] = 1; rob_done[e] = 0; rob_pc[e] = dpc
        rs_valid[free] = 1; rs_pc[free] = dpc; rs_tag[free] = rob_tail
        rs_wait[free] = xor3(dpc % 8, 5)
        if (int(dpc / 8) % 2 && rs_wait[free] < 4) rs_wait[free] += 4
        rob_tail = (rob_tail + 1) % 16
        fq_head = (fq_head + 1) % 4
        dispatched++
      }
      if (fetch) { fq_pc[(fq_head - disp + fq_count + 4) % 4] = next_pc; next_pc++ }
      fq_count += fetch - disp
      if (jump) { fq_count = 0; next_pc = dpc + 3 }
    }
  }
}

FNR == NR { next }

/^eventtrace / {
  delete f
  name = ""
  for (i = 2; i <= NF; i++) {
    if (split($i, kv, "=") == 2) f[kv[1]] = kv[2]
    else if (name == "") name = $i
  }
  c = f["cycle"] + 0
  subject = f["subject"]
  if (name == "insn.fetch") pc_of[subject] = f["pc"]
  else if (name == "uop.dispatch") pc_of[subject] = f["pc"]
  if (!(subject in pc_of)) { fail("record names " subject ", which was never created: " $0); next }
  line = c " " name " " pc_of[subject]
  if (name == "insn.complete") line = line " " f["mispredict"]
  got[++ngot] = line
  if (name == "uop.execute") {
    if (pc_of[subject] + 0 < last_executed) out_of_order++
    last_executed = pc_of[subject] + 0
  }
  if (c > last_cycle) last_cycle = c
}

# Lines of `a` from index 1 to n, sorted within each edge, joined by newlines.
function joined(a, n,    i, j, t, s) {
  for (i = 2; i <= n; i++) {
    t = a[i]
    for (j = i - 1; j >= 1 && (a[j] + 0 > t + 0 || (a[j] + 0 == t + 0 && a[j] > t)); j--) a[j + 1] = a[j]
    a[j + 1] = t
  }
  s = ""
  for (i = 1; i <= n; i++) s = s a[i] "\n"
  return s
}

END {
  model(80)
  w = joined(want, nwant)
  g = joined(got, ngot)
  if (w != g) {
    split(w, wl, "\n"); split(g, gl, "\n")
    for (i = 1; i <= nwant || i <= ngot; i++)
      if (wl[i] != gl[i]) { fail("first difference at line " i ": model has \"" wl[i] "\", trace has \"" gl[i] "\""); break }
  }
  if (last_cycle != 82) fail("the last record is from edge " last_cycle ", not 82; the final cycle was not flushed")
  if (dispatched <= 16) fail("only " dispatched + 0 " dispatches, so the ROB's 16 keys never wrapped")
  if (!rollbacks) fail("no rollback removed a member")
  if (!empty_rollbacks) fail("no rollback was to the tail itself, which removes nothing")
  if (!flushes) fail("no flush emptied a fetch queue that held members")
  if (!out_of_order) fail("no operation executed after a younger one")
  if (!jump_drops) fail("no jump dropped younger fetch-queue entries")
  if (!jumps_alone) fail("no jump found the fetch queue holding only itself, so that it dropped none")
  if (!fq_full) fail("the fetch queue never filled its 4 entries")
  if (!rs_full) fail("the reservation station never filled its 4 slots")
  if (bad) exit 1
  print "PASS: " ngot " records match the model exactly: " dispatched " dispatches wrapping the ROB's keys, " \
    rollbacks " rollbacks removing members and " empty_rollbacks " to the tail itself, " flushes " fetch-queue " \
    "flushes, " jump_drops " jumps dropping younger fetch-queue entries and " jumps_alone " dropping none, " \
    "operations finishing out of order, and the fetch queue and reservation station both full at times"
}
