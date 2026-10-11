package eventtrace

import java.nio.charset.StandardCharsets
import java.nio.file.{Files, Path, Paths}

/** Generates the manifest for tools outside the simulator: what consumers see.
  *
  * It lists each event name and its fields, sorted by name; each enum that a field uses, with its variants; and a
  * sites table mapping each site ID, which every record carries, to its event name, Chisel instance path, and
  * source location. Payload bit layouts are internal and left out.
  */
private[eventtrace] object ManifestGen {
  val FileName = "eventtrace_manifest.json"

  def write(dir: String, events: Seq[EventSchema], sites: Seq[SiteLayout]): Path = {
    val path = Paths.get(dir, FileName)
    Files.createDirectories(path.toAbsolutePath.getParent)
    Files.write(path, json(events, sites).getBytes(StandardCharsets.UTF_8))
    path
  }

  def json(events: Seq[EventSchema], sites: Seq[SiteLayout]): String = {
    val entries = events.map { e =>
      val fields = e.fields.map { f =>
        val enumName = f.enumDef.map(d => s""", "enum": ${quote(d.name)}""").getOrElse("")
        val optional = if (e.optional(f.name)) """, "optional": true""" else ""
        s"""        {"name": ${quote(f.name)}, "type": "${f.kind}"$enumName, "width": ${f.width}$optional}"""
      }
      val fieldList = if (fields.isEmpty) "[]" else fields.mkString("[\n", ",\n", "\n      ]")
      val subject = e.subject.map(s => s"""\n      "subject": ${quote(s)},""").getOrElse("")
      val lifecycle = e.lifecycle.map(l => s"""\n      "lifecycle": ${quote(l)},""").getOrElse("")
      s"""    {
         |      "name": ${quote(e.name)},$subject$lifecycle
         |      "fields": $fieldList
         |    }""".stripMargin
    }
    // Each enum that some field uses, with its variants' names and values in declaration order.
    val enumEntries = sites.flatMap(_.fields.flatMap(_.enumDef)).distinct.sortBy(_.name).map { d =>
      val values = d.variants.map { case (variant, value) => s"${quote(variant)}: $value" }.mkString(", ")
      s"""    {"name": ${quote(d.name)}, "values": {$values}}"""
    }
    // One site per line, so that line-oriented tools can read the table too.
    val siteEntries = sites.sortBy(_.id).map { s =>
      s"""    {"site_id": ${s.id}, "event_name": ${quote(s.name)}, "instance": ${quote(s.instance)}, "source": ${quote(s.location)}}"""
    }
    s"""{
       |  "events": [
       |${entries.mkString(",\n")}
       |  ],
       |  "enums": [
       |${enumEntries.mkString(",\n")}
       |  ],
       |  "sites": [
       |${siteEntries.mkString(",\n")}
       |  ]
       |}
       |""".stripMargin
  }

  private def quote(s: String): String = s.flatMap {
    case '"'           => "\\\""
    case '\\'          => "\\\\"
    case c if c < ' '  => f"\\u${c.toInt}%04x"
    case c             => c.toString
  }.mkString("\"", "", "\"")
}
