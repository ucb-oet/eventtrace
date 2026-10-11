# Stage 1 check: every site fires exactly in the cycles its branch selects, and nowhere else.
# Run as: awk -f check.awk eventtrace_manifest.json run.log
#
# Cycle stamps count clock edges from the start of simulation. TestTop holds reset for edges 0 to 2 and again for
# edges 27 and 28, so records come only from edges 3 to 26 and 29 to 52, and the counter restarts at 0 after each
# reset. Expectations below follow the counter value `count`, not the cycle.
#   "tick" fires every cycle. When count bit 2 is set, exactly one of these also fires:
#   phase 0 -> "first", phase 1 -> "second", phase 2 -> "rest" with path=0, phase 3 -> "rest" with path=1.
#   "lane" is one event in a module instantiated twice: lane0 fires on odd counts, lane1 on even ones.
#   "mode" fires when count bit 3 is set. Its enum field `mode` is count mod 4 named as a Mode variant, or Mode(3),
#   which is not a variant; `target` is the literal Mode.Fast. The manifest must list Mode and both enum fields.
#   "wide" fires when count bit 4 is set; its 76-bit payload spans three words.
#   "flush" fires at count 5 without fields, its site leaving `cause` DontCare, and at count 6 with cause=2. The
#   manifest marks `cause` optional.
# The runtime handles each cycle's records in site-ID order and numbers events as it goes, so records must appear in
# (cycle, site_id) order with event_id counting up from 0. The last cycle's records appear only if the sinks' final
# blocks flush the runtime.
# Each record is assigned a slot (0 to 9) from its event and fields. Its site_id is looked up in the manifest's sites
# table, which must name the record's event and, for the two lanes, their distinct instance paths.
# "rest" fields must print in canonical (sorted) order.
function fail(message) {
  print "FAIL: " message
  bad = 1
}

# The counter value at a cycle, or -1 while reset is asserted.
function count_at(c) {
  if (c >= 3 && c <= 26) return c - 3
  if (c >= 29 && c <= 52) return c - 29
  return -1
}

BEGIN { split("Off Slow Fast Mode(3)", names, " "); for (i = 0; i < 4; i++) mode_text[i] = names[i + 1] }

# Manifest: one site per line in the sites table, one enum per line in the enums table.
FNR == NR {
  if (index($0, "{\"name\": \"Mode\", \"values\": {\"Off\": 0, \"Slow\": 1, \"Fast\": 2}}")) mode_enum = 1
  if (index($0, "{\"name\": \"mode\", \"type\": \"Enum\", \"enum\": \"Mode\", \"width\": 2}")) mode_fields++
  if (index($0, "{\"name\": \"target\", \"type\": \"Enum\", \"enum\": \"Mode\", \"width\": 2}")) mode_fields++
  if (index($0, "{\"name\": \"cause\", \"type\": \"UInt\", \"width\": 2, \"optional\": true}")) cause_optional = 1
  if (match($0, /"site_id": [0-9]+/)) {
    id = substr($0, RSTART + 11, RLENGTH - 11) + 0
    match($0, /"event_name": "[^"]*"/); event_of[id] = substr($0, RSTART + 15, RLENGTH - 16)
    match($0, /"instance": "[^"]*"/);   instance_of[id] = substr($0, RSTART + 13, RLENGTH - 14)
    sites++
  }
  next
}

/^eventtrace / {
  delete f
  name = ""; at = 0
  for (i = 2; i <= NF; i++) {
    if (split($i, kv, "=") == 2) f[kv[1]] = kv[2]
    else if (name == "") { name = $i; at = i }
  }
  c = f["cycle"] + 0
  k = count_at(c)
  id = f["site_id"]
  if (!(id in event_of)) { fail("record names site " id ", which the manifest lacks: " $0); next }
  if (event_of[id] != name) fail("record from site " id " is \"" name "\", but the manifest says \"" event_of[id] "\"")
  if (k < 0) { fail("record while reset is asserted: " $0); next }
  if (f["event_id"] != n) fail("event_id out of sequence, expected " n ": " $0)
  if (n > 0 && (c < prev_cycle || (c == prev_cycle && id + 0 <= prev_site))) fail("record out of (cycle, site_id) order: " $0)
  prev_cycle = c; prev_site = id + 0

  if (name == "tick")        slot = 0
  else if (name == "first")  slot = 1
  else if (name == "second") slot = 2
  else if (name == "rest")   slot = (f["path"] == 0 ? 3 : 4)
  else if (name == "lane")   slot = (instance_of[id] == "Conditional.lane0" ? 5 : instance_of[id] == "Conditional.lane1" ? 6 : -1)
  else if (name == "mode")   slot = 7
  else if (name == "wide")   slot = 8
  else if (name == "flush")  slot = 9
  else                       slot = -1
  seen[c, slot]++
  n++
  fields = NF - at
  if (slot == 0)      ok = (fields == 0)
  else if (slot == 1) ok = (f["count"] == k)
  else if (slot == 2) ok = (f["count"] == k && f["sum"] == (k + 3) % 256)
  else if (slot == 3) ok = (f["phase"] == 2 && f["odd"] == 0 && f["path"] == 0)
  else if (slot == 4) ok = (f["phase"] == 3 && f["odd"] == 1 && f["path"] == 1)
  else if (slot == 5) ok = (f["value"] == k)
  else if (slot == 6) ok = (f["value"] == k + 1)
  else if (slot == 7) ok = (f["mode"] == mode_text[k % 4] && f["target"] == "Fast" && fields == 2)
  else if (slot == 8) ok = (f["a"] == k * 16843009 && f["b"] == (k % 16) * 4294967296 + k * 16843009 && f["c"] == k)
  else if (slot == 9) ok = (k == 5 && fields == 0) || (k == 6 && fields == 1 && f["cause"] == 2)
  else                ok = 0
  if (slot == 3 || slot == 4)
    if (!($(at + 1) ~ /^odd=/ && $(at + 2) ~ /^path=/ && $(at + 3) ~ /^phase=/ && fields == 3)) ok = 0
  if (!ok) fail("unexpected record: " $0)
}

END {
  if (sites != 11) fail("expected 11 sites in the manifest, got " sites + 0)
  if (!cause_optional) fail("the manifest should mark flush's cause optional")
  if (!mode_enum) fail("the manifest's enums table lacks Mode with its three variants")
  if (mode_fields != 2) fail("the manifest should list mode and target as Mode enum fields")
  for (id in event_of) {
    if (event_of[id] == "lane") lanes[instance_of[id]]++
    else if (instance_of[id] != "Conditional") fail("site " id " should be in Conditional, not " instance_of[id])
  }
  if (lanes["Conditional.lane0"] != 1 || lanes["Conditional.lane1"] != 1)
    fail("expected one lane site in each of Conditional.lane0 and Conditional.lane1")
  if (n == 0) { fail("no records"); exit 1 }
  for (c = 0; c <= 52; c++) {
    k = count_at(c)
    branch = (k >= 0 && int(k / 4) % 2 == 1) ? k % 4 + 1 : -1
    for (slot = 0; slot <= 9; slot++) {
      want = (k >= 0 && (slot == 0 || slot == branch || (slot == 5 && k % 2 == 1) || (slot == 6 && k % 2 == 0) ||
              (slot == 7 && int(k / 8) % 2 == 1) || (slot == 8 && int(k / 16) % 2 == 1) ||
              (slot == 9 && (k == 5 || k == 6)))) ? 1 : 0
      if (seen[c, slot] + 0 != want)
        fail("cycle " c " slot " slot ": expected " want " record(s), got " seen[c, slot] + 0)
    }
  }
  if (bad) exit 1
  print "PASS: " n " records from " sites " sites over edges 0 to 52, with reset at edges 0-2 and 27-28; every site " \
    "fired exactly in its branch's cycles, the two lane instances have their own sites, enum fields print as " \
    "variant names, a three-word payload decodes, a DontCare field is left out, and event IDs follow (cycle, site) " \
    "order through the final cycle"
}
