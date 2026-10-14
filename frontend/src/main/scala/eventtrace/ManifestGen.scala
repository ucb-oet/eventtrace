package eventtrace

/** Generates the manifest for tools outside the simulator: what consumers see.
  *
  * It lists each event name and its fields, sorted by name; each object type and its fields, in declaration order,
  * each with its property, `const`, `setOnce`, or `mutable`; each enum that a field uses, with its variants; and a
  * sites table mapping each site ID, which every record carries, to its event name, Chisel instance path, and
  * source location. Payload bit layouts are internal and left out.
  *
  * Unlike the generated C++, the manifest describes the design rather than being compiled into the simulator, so it
  * does not travel through firtool; see [[EventTrace.manifest]].
  */
private[eventtrace] object ManifestGen {
  def json(events: Seq[EventSchema], sites: Seq[SiteLayout], objectTypes: Seq[ObjectTypeInfo]): String = {
    // A field's name and type; an object ID names its object type instead of a width.
    def field(name: String, t: FieldType, more: String): String = {
      val detail = t.idOf match {
        case Some(o) => s""", "object": ${quote(o)}"""
        case None    => t.enumDef.map(d => s""", "enum": ${quote(d.name)}""").getOrElse("") + s""", "width": ${t.width}"""
      }
      s"""        {"name": ${quote(name)}, "type": "${t.kind}"$detail$more}"""
    }
    def list(items: Seq[String]): String = if (items.isEmpty) "[]" else items.mkString("[\n", ",\n", "\n      ]")
    val entries = events.map { e =>
      val fields = e.fields.map { f =>
        field(f.name, f.tpe.get, if (e.optional(f.name)) """, "optional": true""" else "")
      }
      val fieldList = list(fields)
      val subject = e.subject.map(s => s"""\n      "subject": ${quote(s)},""").getOrElse("")
      val lifecycle = e.lifecycle.map(l => s"""\n      "lifecycle": ${quote(l)},""").getOrElse("")
      s"""    {
         |      "name": ${quote(e.name)},$subject$lifecycle
         |      "fields": $fieldList
         |    }""".stripMargin
    }
    val objectEntries = objectTypes.sortBy(_.name).map { t =>
      val fields = t.fields.map { f =>
        val property = f.property match {
          case FieldProperty.Const   => "const"
          case FieldProperty.SetOnce => "setOnce"
          case FieldProperty.Mutable => "mutable"
        }
        field(f.name, f.tpe, s""", "property": "$property"""")
      }
      s"""    {
         |      "name": ${quote(t.name)},
         |      "fields": ${list(fields)}
         |    }""".stripMargin
    }
    // Each enum that some field uses, with its variants' names and values in declaration order.
    val enumDefs = sites.flatMap(_.fields.flatMap(_.enumDef)) ++ objectTypes.flatMap(_.fields.flatMap(_.tpe.enumDef))
    val enumEntries = enumDefs.distinct.sortBy(_.name).map { d =>
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
       |  "objects": [
       |${objectEntries.mkString(",\n")}
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

  /** The SHA-256 of a manifest's UTF-8 text, in hex, which the trace's header carries. */
  def sha256(manifest: String): String =
    java.security.MessageDigest.getInstance("SHA-256")
      .digest(manifest.getBytes(java.nio.charset.StandardCharsets.UTF_8))
      .map(b => f"${b & 0xff}%02x")
      .mkString

  private def quote(s: String): String = s.flatMap {
    case '"'           => "\\\""
    case '\\'          => "\\\\"
    case c if c < ' '  => f"\\u${c.toInt}%04x"
    case c             => c.toString
  }.mkString("\"", "", "\"")
}
