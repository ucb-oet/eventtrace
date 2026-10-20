# Collections check: every record, in order, matches a model of the pipeline exactly.
# Run as: awk -f check.awk eventtrace_manifest.json run.log
#
# Records come from edges 3 to 62, where the counter k is the edge minus 3, from 0 to 59. The model keeps the
# pipeline registers and which instruction each stage holds. Instructions join the collection when created, so join
# order is ID order, and before each edge the collection holds exactly the instructions in writeback, execute, and
# decode, oldest first. Within a cycle, records follow site order, and each site's records follow its view's order.
# A record whose test fails is dropped and takes no event ID, so event IDs count only the records expected here.
function fail(message) {
  print "FAIL: " message
  bad = 1
}

# One expected record of instruction `id` at site `site`; `fields` are its fields sorted by name, if any.
function emit(site, name, id, fields) {
  want[m] = sprintf("eventtrace cycle=%d event_id=%d site_id=%d instruction.%s subject=instruction#%d%s",
                    k + 3, m, site, name, id, fields == "" ? "" : " " fields)
  m++
  seen[name]++
}

# One expected record of request `id` at site `site`; `fields` are its fields sorted by name, if any.
function emit_request(site, name, id, fields) {
  want[m] = sprintf("eventtrace cycle=%d event_id=%d site_id=%d request.%s subject=request#%d%s",
                    k + 3, m, site, name, id, fields == "" ? "" : " " fields)
  m++
  seen["request." name]++
}

# One expected record of the subject-less event "idle".
function emit_idle(site) {
  want[m] = sprintf("eventtrace cycle=%d event_id=%d site_id=%d idle count=%d", k + 3, m, site, k)
  m++
  seen["idle"]++
}

FNR == NR {
  manifest = manifest $0 "\n"
  next
}

/^eventtrace / { got[n++] = $0 }

END {
  m = 0; next_id = 0
  decV = 0; exeV = 0; wbV = 0; was_stalled = 0; pending = 0; next_request = 0
  for (k = 0; k <= 59; k++) {
    kill  = exeV && k % 11 == 5
    stall = decV && exeV && !kill && k % 7 == 3
    issue = decV && !stall && !kill
    enter = !stall && !kill

    # The collection before the edge, in join order.
    members = 0
    if (wbV)  member[members++] = wbId
    if (exeV) member[members++] = exeId
    if (decV) member[members++] = decId

    if (enter) { new_id = next_id++; emit(0, "decode", new_id, sprintf("odd=%d pc=%d stage=Decode", k % 2, k)) }
    if (issue) emit(1, "issue", decId, "stage=Execute")
    if (exeV)  emit(2, "writeback", exeId, sprintf("result=%d stage=Writeback", (exePc + 1) % 256))
    if (wbV)   emit(3, "retire", member[0], "")
    if (kill && decV) emit(4, "squash", decId, sprintf("by=instruction#%d", exeId))
    if (stall && !was_stalled) emit(5, "stall", decId, sprintf("by=instruction#%d", exeId))
    if (k % 5 == 0) {
      for (pass = 0; pass <= 1; pass++)
        for (odd = 0; odd <= 1; odd++)
          for (i = 0; i < members; i++)
            if (pc[member[i]] % 2 == odd) emit(6 + pass, "sample", member[i], "pass=" pass)
    }
    if (k % 4 == 2 && members >= 2) {
      emit(8, "second", member[1], "")
      emit(9, "youngest", member[members - 1], "")
    }
    # Execute holds an instruction exactly when exeV, so the test on its size passes then; the nested test passes
    # when the pipeline is full.
    if (k % 3 == 0) {
      if (exeV) {
        emit(10, "busy", exeId, "")
        if (members >= 3) for (i = 0; i < members; i++) emit(11, "crowd", member[i], "")
      } else emit_idle(12)
    }
    # A request names the instruction in execute that issued it, and is done two cycles later.
    issue_request = exeV && k % 4 == 1
    request_done = pending && k % 4 == 3
    if (issue_request) emit_request(13, "issue", next_request++, sprintf("insn=instruction#%d", exeId))
    if (request_done) emit_request(14, "done", next_request - 1, "")
    if (k % 5 == 1 && decV) emit(15, "pick", int(k / 2) % 2 ? member[0] : decId, "")

    # The edge.
    if (enter) pc[new_id] = k
    wbV = exeV; wbId = exeId
    exeV = issue; exeId = decId; exePc = decPc
    decV = !kill
    if (enter) { decId = new_id; decPc = k }
    was_stalled = stall
    if (issue_request) pending = 1
    if (request_done) pending = 0
  }

  if (n != m) fail("expected " m " records, got " n)
  for (i = 0; i < m && i < n; i++)
    if (got[i] != want[i]) { fail("record " i ": expected \"" want[i] "\", got \"" got[i] "\""); break }

  split("\"name\": \"instruction.retire\",|\"lifecycle\": \"complete\",|\"lifecycle\": \"abort\",|" \
        "{\"name\": \"by\", \"type\": \"Id\", \"object\": \"instruction\", \"role\": \"blocked_by\"}|" \
        "{\"name\": \"by\", \"type\": \"Id\", \"object\": \"instruction\", \"role\": \"aborted_by\"}|" \
        "{\"name\": \"insn\", \"type\": \"Id\", \"object\": \"instruction\", \"property\": \"const\", \"role\": \"parent\"}|" \
        "\"event_name\": \"instruction.stall\", |\"blocks\": true}|" \
        "\"name\": \"request.issue\",|{\"name\": \"insn\", \"type\": \"Id\", \"object\": \"instruction\"}|" \
        "{\"name\": \"stage\", \"type\": \"Enum\", \"enum\": \"Stage\", \"width\": 2}|" \
        "{\"name\": \"Stage\", \"values\": {\"Decode\": 0, \"Execute\": 1, \"Writeback\": 2}}", entries, "|")
  for (i in entries)
    if (!index(manifest, entries[i])) fail("the manifest lacks " entries[i])
  split("decode issue writeback retire squash stall sample second youngest busy crowd idle request.issue request.done pick",
        names, " ")
  for (i in names)
    if (!seen[names[i]]) fail("the model never expects " names[i] ", so the run does not cover it")

  if (bad) exit 1
  print "PASS: " n " records match the pipeline model exactly: " seen["decode"] " instructions created, " \
    seen["retire"] " retired and " seen["squash"] " squashed through queries on one collection, " seen["stall"] \
    " stalls naming their blockers, " seen["sample"] " samples in ranked order, and tests on view sizes keeping " \
    seen["busy"] " busy, " seen["crowd"] " crowd, and " seen["idle"] " idle records, and " seen["request.issue"] \
    " requests keeping the ID of the instruction that issued them; " seen["pick"] " picks by a query with a " \
    "hardware Bool as a term"
}
