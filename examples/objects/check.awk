# Objects check: every record, in order, matches the expected sequence exactly.
# Run as: awk -f check.awk eventtrace_manifest.json run.log
#
# Records come from edges 3 to 51, where the counter is the edge minus 3, from 0 to 48. Within a cycle the runtime
# handles sites in order: site 0 creates a packet when count mod 4 is 0, site 1 another when count mod 8 is 0, and
# site 2 a token when count mod 3 is 0. Event IDs count every record from 0; packets and tokens each count from 0.
# Site 1 leaves kind Absent, so its records have no kind; lane is a literal at both packet sites.
function fail(message) {
  print "FAIL: " message
  bad = 1
}

FNR == NR {
  manifest = manifest $0 "\n"
  next
}

/^eventtrace / { got[n++] = $0 }

END {
  m = 0; packets = 0; tokens = 0
  for (k = 0; k <= 48; k++) {
    c = k + 3
    if (k % 4 == 0) {
      want[m] = sprintf("eventtrace cycle=%d event_id=%d site_id=0 packet.arrive subject=packet#%d kind=%d lane=0 seq=%d",
                        c, m, packets++, int(k / 4) % 4, k)
      m++
    }
    if (k % 8 == 0) {
      want[m] = sprintf("eventtrace cycle=%d event_id=%d site_id=1 packet.arrive subject=packet#%d lane=1 seq=%d",
                        c, m, packets++, k)
      m++
    }
    if (k % 3 == 0) {
      want[m] = sprintf("eventtrace cycle=%d event_id=%d site_id=2 token.grant subject=token#%d slot=%d",
                        c, m, tokens++, k % 16)
      m++
    }
  }
  if (n != m) fail("expected " m " records, got " n)
  for (i = 0; i < m && i < n; i++)
    if (got[i] != want[i]) { fail("record " i ": expected \"" want[i] "\", got \"" got[i] "\""); break }

  split("\"name\": \"packet.arrive\",|\"subject\": \"packet\",|\"lifecycle\": \"create\",|" \
        "{\"name\": \"kind\", \"type\": \"UInt\", \"width\": 2, \"optional\": true}|" \
        "{\"name\": \"lane\", \"type\": \"UInt\", \"width\": 1}|{\"name\": \"seq\", \"type\": \"UInt\", \"width\": 8}|" \
        "\"name\": \"token.grant\",|\"subject\": \"token\",|{\"name\": \"slot\", \"type\": \"UInt\", \"width\": 4}|" \
        "\"event_name\": \"packet.arrive\"|\"event_name\": \"token.grant\"", entries, "|")
  for (i in entries)
    if (!index(manifest, entries[i])) fail("the manifest lacks " entries[i])

  if (bad) exit 1
  print "PASS: " n " records match exactly: " packets " packets and " tokens " tokens numbered per type in site " \
    "order, one event counter, kind left out where Absent, and the last cycle flushed"
}
