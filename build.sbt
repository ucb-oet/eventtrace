// Standalone EventTrace build, pinned to Chipyard's toolchains: Chisel 6 by default, Chisel 7 with USE_CHISEL7,
// matching Chipyard's own build.sbt.
val useChisel7 = sys.env.contains("USE_CHISEL7")
val chiselVersion = if (useChisel7) "7.13.0" else "6.7.0"

ThisBuild / scalaVersion := (if (useChisel7) "2.13.18" else "2.13.16")
ThisBuild / organization := "edu.berkeley.cs"

lazy val chiselSettings = Seq(
  libraryDependencies += "org.chipsalliance" %% "chisel" % chiselVersion,
  addCompilerPlugin("org.chipsalliance" % "chisel-plugin" % chiselVersion cross CrossVersion.full),
  scalacOptions ++= Seq("-deprecation", "-feature", "-unchecked", "-language:reflectiveCalls")
)

lazy val frontend = (project in file("frontend"))
  .settings(chiselSettings)
  .settings(libraryDependencies += "com.lihaoyi" %% "sourcecode" % "0.3.1")

lazy val stage0 = (project in file("examples/stage0"))
  .dependsOn(frontend)
  .settings(chiselSettings)

lazy val stage1 = (project in file("examples/stage1"))
  .dependsOn(frontend)
  .settings(chiselSettings)

lazy val objects = (project in file("examples/objects"))
  .dependsOn(frontend)
  .settings(chiselSettings)

lazy val collections = (project in file("examples/collections"))
  .dependsOn(frontend)
  .settings(chiselSettings)
