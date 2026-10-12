# Stage 0b check: one "tick" record every eighth cycle, with both fields sliced correctly.
# Cycle stamps count clock edges from the start of simulation; reset holds for edges 0 to 2, so the first record,
# with the counter's reset value, comes from edge 3.
# The counter starts at 0xffffffe0, and `delta` is its low byte as a signed 8-bit value minus 20.

# The value of a hex number, as the dump writes a UInt wider than 16 bits.
function hex(text,    n, i) {
  n = 0
  for (i = 3; i <= length(text); i++) n = n * 16 + index("0123456789abcdef", substr(text, i, 1)) - 1
  return n
}

/^eventtrace / {
  delete f
  name = ""
  for (i = 2; i <= NF; i++) {
    if (split($i, kv, "=") == 2) f[kv[1]] = (kv[2] ~ /^0x/ ? hex(kv[2]) : kv[2])
    else if (name == "") name = $i
  }
  cycle = 3 + n * 8
  count = (4294967264 + n * 8) % 4294967296
  low = count % 256
  delta = (low >= 128 ? low - 256 : low) - 20
  if (delta < -128) delta += 256
  if (f["cycle"] != cycle || f["event_id"] != n || f["site_id"] != 0 || name != "tick" || f["count"] != count ||
      f["delta"] != delta) {
    printf "FAIL: expected cycle=%d event_id=%d site_id=0 tick count=0x%08x delta=%d, got: %s\n", cycle, n, count, delta, $0
    bad = 1
  }
  n++
}
END {
  if (n == 0) { print "FAIL: no records"; exit 1 }
  if (bad) exit 1
  print "PASS: " n " records, every eighth cycle, delta and count sliced correctly"
}
