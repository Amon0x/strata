plugins {
    base
}

version = "0.1.0"
group = "dev.strata"

base {
    archivesName.set("strata")
}

val windowsCmake = "C:/Program Files/CMake/bin/cmake.exe"
val windowsCtest = "C:/Program Files/CMake/bin/ctest.exe"
val nativeBuildDirectory = layout.buildDirectory.dir("native/windows-x64")

tasks.register("checkDemoDsl") {
    group = "verification"
    description = "Checks the bundled settings, showcase, and F8 modules with the native compiler."
    dependsOn("validateStrataModules")
}

tasks.register<Exec>("generateStrataAuthoring") {
    group = "documentation"
    description = "Regenerates the registry-derived .strata reference, completions, and TextMate grammar."
    dependsOn("buildNative")
    doFirst {
        commandLine(
            nativeBuildDirectory.get().file("RelWithDebInfo/strata_authoring.exe").asFile.absolutePath,
            "--write",
            layout.projectDirectory.asFile.absolutePath,
        )
    }
}

tasks.register<Exec>("checkStrataAuthoring") {
    group = "verification"
    description = "Fails when native registry, lexical, completion, grammar, reference, or diagnostic artifacts are stale."
    dependsOn("buildNative")
    inputs.files(fileTree("native") {
        include("**/*.c", "**/*.cpp", "**/*.h", "**/*.hpp")
    })
    inputs.files(
        "docs/generated/strata-reference.md",
        "docs/generated/diagnostics.md",
        "editor/strata-completions.json",
        "editor/vscode/syntaxes/strata.tmLanguage.json",
        "src/main/resources/strata/registry-v1.json",
        "src/main/resources/strata/lexical-v1.json",
    )
    doFirst {
        commandLine(
            nativeBuildDirectory.get().file("RelWithDebInfo/strata_authoring.exe").asFile.absolutePath,
            "--check",
            layout.projectDirectory.asFile.absolutePath,
        )
    }
}

val configureNative = tasks.register<Exec>("configureNative") {
    group = "build"
    description = "Configures the portable C++23 runtime with the required Windows preview toolchain."
    inputs.files(fileTree("native"))
    outputs.file(nativeBuildDirectory.map { it.file("CMakeCache.txt") })
    doFirst {
        commandLine(
            windowsCmake,
            "-S", layout.projectDirectory.dir("native").asFile.absolutePath,
            "-B", nativeBuildDirectory.get().asFile.absolutePath,
            "-G", "Visual Studio 18 2026",
            "-A", "x64",
            "-T", "version=14.52",
            "-DCMAKE_CONFIGURATION_TYPES=Debug;RelWithDebInfo",
            "-DSTRATA_BUILD_TESTS=ON",
            "-DSTRATA_BUILD_TOOLS=ON",
            "-DSTRATA_BUILD_SAMPLES=ON",
            "-DSTRATA_WARNINGS_AS_ERRORS=ON",
        )
    }
}

val buildNative = tasks.register<Exec>("buildNative") {
    group = "build"
    description = "Builds strata_core, the stable strata_c ABI, tools, tests, and embedding samples."
    dependsOn(configureNative)
    inputs.files(fileTree("native"))
    outputs.files(
        nativeBuildDirectory.map { it.file("RelWithDebInfo/strata_c.dll") },
        nativeBuildDirectory.map { it.file("RelWithDebInfo/strata_core_tests.exe") },
        nativeBuildDirectory.map { it.file("RelWithDebInfo/strata_data_tests.exe") },
        nativeBuildDirectory.map { it.file("RelWithDebInfo/strata_compiler_tests.exe") },
        nativeBuildDirectory.map { it.file("RelWithDebInfo/strata_abi_tests.exe") },
        nativeBuildDirectory.map { it.file("RelWithDebInfo/strata_compile.exe") },
        nativeBuildDirectory.map { it.file("RelWithDebInfo/strata_authoring.exe") },
        nativeBuildDirectory.map { it.file("RelWithDebInfo/strata_headless.exe") },
        nativeBuildDirectory.map { it.file("RelWithDebInfo/strata_desktop.exe") },
    )
    doFirst {
        commandLine(
            windowsCmake,
            "--build", nativeBuildDirectory.get().asFile.absolutePath,
            "--config", "RelWithDebInfo",
            "--parallel",
        )
    }
}

val benchmarkDesktop = tasks.register<Exec>("benchmarkDesktop") {
    group = "benchmark"
    description = "Runs the visible uncapped Win32/D3D11 showcase performance scenario."
    dependsOn(buildNative)
    doFirst {
        val output = layout.buildDirectory.dir("performance/showcase-desktop").get().asFile
        output.deleteRecursively()
        val arguments = mutableListOf(
            nativeBuildDirectory.get().file("RelWithDebInfo/strata_desktop.exe").asFile.absolutePath,
            "--performance",
            layout.projectDirectory.file("native/tests/fixtures/performance/showcase.json").asFile.absolutePath,
            "--output",
            output.absolutePath,
        )
        providers.gradleProperty("performanceBaseline").orNull?.let {
            arguments += listOf("--baseline", file(it).absolutePath)
        }
        arguments += layout.projectDirectory.dir("src/main/resources").asFile.absolutePath
        commandLine(arguments)
    }
}

val checkNative = tasks.register<Exec>("checkNative") {
    group = "verification"
    description = "Runs native unit, ABI, regression, and C/C++ embedding tests through CTest."
    dependsOn(buildNative)
    inputs.files(fileTree("native"))
    doFirst {
        commandLine(
            windowsCtest,
            "--test-dir", nativeBuildDirectory.get().asFile.absolutePath,
            "--build-config", "RelWithDebInfo",
            "--output-on-failure",
        )
    }
}

val checkReleaseNativeDependencies = tasks.register("checkReleaseNativeDependencies") {
    group = "verification"
    description = "Rejects sanitizer-linked DLLs from the ordinary native package."
    dependsOn(buildNative)
    val cLibrary = nativeBuildDirectory.map { it.file("RelWithDebInfo/strata_c.dll") }
    inputs.file(cLibrary)
    doLast {
        val image = cLibrary.get().asFile.readBytes().toString(Charsets.ISO_8859_1)
        check("clang_rt.asan_dynamic" !in image) {
            "The ordinary RelWithDebInfo native DLL must not import the ASAN runtime"
        }
    }
}

val nativeInstallDirectory = layout.buildDirectory.dir("native/install/windows-x64")
val nativeInstalledSmokeBuildDirectory = layout.buildDirectory.dir("native/installed-smoke/windows-x64")

val installNative = tasks.register<Exec>("installNative") {
    group = "build"
    description = "Installs the public Strata C ABI package and standalone consumer fixtures."
    dependsOn(buildNative)
    inputs.files(buildNative)
    inputs.files(fileTree("native/include"))
    inputs.files(fileTree("native/samples"))
    inputs.files(fileTree("native/cmake"))
    inputs.file("native/CMakeLists.txt")
    inputs.file("src/main/resources/strata/registry-v1.json")
    inputs.files(fileTree("src/main/resources/assets/strata"))
    outputs.dir(nativeInstallDirectory)
    doFirst {
        commandLine(
            windowsCmake,
            "--install", nativeBuildDirectory.get().asFile.absolutePath,
            "--config", "RelWithDebInfo",
            "--prefix", nativeInstallDirectory.get().asFile.absolutePath,
        )
    }
}

val configureInstalledNativeSmoke = tasks.register<Exec>("configureInstalledNativeSmoke") {
    group = "verification"
    description = "Configures C and C++ consumers using only the installed Strata package."
    dependsOn(installNative)
    inputs.files(
        "native/samples/installed/CMakeLists.txt",
        "native/samples/c_smoke.c",
        "native/samples/cpp_smoke.cpp",
        "native/samples/desktop_app.cpp",
        "native/samples/desktop_app.json",
        "native/cmake/StrataConfig.cmake.in",
    )
    inputs.files(fileTree("native/include"))
    outputs.file(nativeInstalledSmokeBuildDirectory.map { it.file("CMakeCache.txt") })
    doFirst {
        commandLine(
            windowsCmake,
            "-S", nativeInstallDirectory.get().dir("share/strata/samples").asFile.absolutePath,
            "-B", nativeInstalledSmokeBuildDirectory.get().asFile.absolutePath,
            "-G", "Visual Studio 18 2026",
            "-A", "x64",
            "-T", "version=14.52",
            "-DCMAKE_PREFIX_PATH=${nativeInstallDirectory.get().asFile.absolutePath}",
        )
    }
}

val buildInstalledNativeSmoke = tasks.register<Exec>("buildInstalledNativeSmoke") {
    group = "verification"
    description = "Builds standalone installed-package C and C++ consumers."
    dependsOn(configureInstalledNativeSmoke)
    doFirst {
        commandLine(
            windowsCmake,
            "--build", nativeInstalledSmokeBuildDirectory.get().asFile.absolutePath,
            "--config", "RelWithDebInfo",
            "--parallel",
        )
    }
}

val checkInstalledNativeSmoke = tasks.register<Exec>("checkInstalledNativeSmoke") {
    group = "verification"
    description = "Runs standalone C and C++ consumers against the installed Strata DLL."
    dependsOn(buildInstalledNativeSmoke)
    doFirst {
        commandLine(
            windowsCtest,
            "--test-dir", nativeInstalledSmokeBuildDirectory.get().asFile.absolutePath,
            "--build-config", "RelWithDebInfo",
            "--output-on-failure",
        )
    }
}

fun registerStrataValidationTask(name: String, module: String, schemas: String) = tasks.register<Exec>(name) {
    group = "verification"
    description = "Validates $module through the standalone .strata module compiler."
    dependsOn("buildNative")
    inputs.files(fileTree(layout.projectDirectory.dir("src/main/resources/assets/strata/ui")) { include("**/*.strata") })
    inputs.file(schemas)
    inputs.file("src/main/resources/strata/registry-v1.json")
    doFirst {
        commandLine(
            nativeBuildDirectory.get().file("RelWithDebInfo/strata_compile.exe").asFile.absolutePath,
            "--check-module",
            layout.projectDirectory.file(module).asFile.absolutePath,
            layout.projectDirectory.file("src/main/resources/strata/registry-v1.json").asFile.absolutePath,
            layout.projectDirectory.file(schemas).asFile.absolutePath,
        )
    }
}

tasks.register<Exec>("validateStrata") {
    group = "verification"
    description = "Validates -PstrataFile=<module.strata> with optional -PstrataSchemas=<schemas.json>."
    dependsOn("buildNative")
    doFirst {
        val module = providers.gradleProperty("strataFile").orNull
            ?: throw GradleException("validateStrata requires -PstrataFile=<module.strata>")
        val schemas = providers.gradleProperty("strataSchemas").orNull
        val arguments = mutableListOf(
            nativeBuildDirectory.get().file("RelWithDebInfo/strata_compile.exe").asFile.absolutePath,
            "--check-module",
            layout.projectDirectory.file(module).asFile.absolutePath,
            layout.projectDirectory.file("src/main/resources/strata/registry-v1.json").asFile.absolutePath,
        )
        if (schemas != null) arguments += layout.projectDirectory.file(schemas).asFile.absolutePath
        commandLine(arguments)
    }
}

val validateShowcaseStrata = registerStrataValidationTask(
    "validateShowcaseStrata",
    "src/main/resources/assets/strata/ui/demo_surface.strata",
    "src/main/resources/assets/strata/ui/demo_surface.schemas.json",
)
val validateSettingsStrata = registerStrataValidationTask(
    "validateSettingsStrata",
    "src/main/resources/assets/strata/ui/settings_app.strata",
    "src/main/resources/assets/strata/ui/settings_app.schemas.json",
)
val validateDebugStrata = registerStrataValidationTask(
    "validateDebugStrata",
    "src/main/resources/assets/strata/ui/debug_overlay.strata",
    "src/main/resources/assets/strata/ui/debug_overlay.schemas.json",
)
val validateHubStrata = registerStrataValidationTask(
    "validateHubStrata",
    "src/main/resources/assets/strata/ui/strata_hub.strata",
    "src/main/resources/assets/strata/ui/strata_hub.schemas.json",
)

data class BundledStrataModule(
    val name: String,
    val source: String,
    val schemas: String,
    val artifact: String,
)

val bundledStrataModules = listOf(
    BundledStrataModule(
        "Showcase",
        "src/main/resources/assets/strata/ui/demo_surface.strata",
        "src/main/resources/assets/strata/ui/demo_surface.schemas.json",
        "src/main/resources/assets/strata/ui/demo_surface.compiled.bin",
    ),
    BundledStrataModule(
        "Settings",
        "src/main/resources/assets/strata/ui/settings_app.strata",
        "src/main/resources/assets/strata/ui/settings_app.schemas.json",
        "src/main/resources/assets/strata/ui/settings_app.compiled.bin",
    ),
    BundledStrataModule(
        "Debug",
        "src/main/resources/assets/strata/ui/debug_overlay.strata",
        "src/main/resources/assets/strata/ui/debug_overlay.schemas.json",
        "src/main/resources/assets/strata/ui/debug_overlay.compiled.bin",
    ),
    BundledStrataModule(
        "PerformanceHud",
        "src/main/resources/assets/strata/ui/performance_hud.strata",
        "src/main/resources/assets/strata/ui/performance_hud.schemas.json",
        "src/main/resources/assets/strata/ui/performance_hud.compiled.bin",
    ),
    BundledStrataModule(
        "Hub",
        "src/main/resources/assets/strata/ui/strata_hub.strata",
        "src/main/resources/assets/strata/ui/strata_hub.schemas.json",
        "src/main/resources/assets/strata/ui/strata_hub.compiled.bin",
    ),
)

fun registerBundledArtifactTask(
    taskName: String,
    descriptionText: String,
    mode: String,
    module: BundledStrataModule,
) = tasks.register<Exec>(taskName) {
    group = if (mode == "--check-artifact") "verification" else "build"
    description = descriptionText
    dependsOn("buildNative")
    inputs.files(
        fileTree(layout.projectDirectory.dir("src/main/resources/assets/strata/ui")) {
            include("**/*.strata")
        },
    )
    inputs.files(module.schemas, "src/main/resources/strata/registry-v1.json")
    if (mode == "--check-artifact") inputs.file(module.artifact)
    if (mode == "--emit-artifact") outputs.file(module.artifact)
    doFirst {
        commandLine(
            nativeBuildDirectory.get().file("RelWithDebInfo/strata_compile.exe").asFile.absolutePath,
            mode,
            layout.projectDirectory.file(module.source).asFile.absolutePath,
            layout.projectDirectory.file("src/main/resources/strata/registry-v1.json").asFile.absolutePath,
            layout.projectDirectory.file(module.schemas).asFile.absolutePath,
            layout.projectDirectory.dir("src/main/resources").asFile.absolutePath,
            layout.projectDirectory.file(module.artifact).asFile.absolutePath,
        )
    }
}

val generateBundledStrataArtifacts = bundledStrataModules.map { module ->
    registerBundledArtifactTask(
        "generate${module.name}StrataArtifact",
        "Regenerates the canonical compiled artifact for ${module.source}.",
        "--emit-artifact",
        module,
    )
}
val checkBundledStrataArtifacts = bundledStrataModules.map { module ->
    registerBundledArtifactTask(
        "check${module.name}StrataArtifact",
        "Fails when ${module.artifact} is stale.",
        "--check-artifact",
        module,
    )
}

tasks.register("generateBundledStrataArtifacts") {
    group = "build"
    description = "Regenerates every checked-in bundled .strata compiled artifact."
    dependsOn(generateBundledStrataArtifacts)
}

tasks.register("checkBundledStrataArtifacts") {
    group = "verification"
    description = "Verifies every bundled .strata compiled artifact is current."
    dependsOn(checkBundledStrataArtifacts)
}

tasks.register("validateStrataModules") {
    group = "verification"
    description = "Runs standalone validation for every bundled .strata application module."
    dependsOn(validateShowcaseStrata, validateSettingsStrata, validateDebugStrata, validateHubStrata)
}

tasks.register("checkNativeCoreCutover") {
    group = "verification"
    description = "Prevents the retired Kotlin/Swing core and Gradle oracle from returning."
    val boundaryFiles = files(
        "settings.gradle.kts",
        "build.gradle.kts",
    )
    inputs.files(boundaryFiles)
    doLast {
        check(fileTree("src/main/kotlin").files.isEmpty()) { "The retired Kotlin platform-neutral core has returned" }
        check(fileTree("src/test/kotlin").files.isEmpty()) { "The retired Kotlin oracle test tree has returned" }
        check(fileTree("desktop/src").files.isEmpty()) { "The retired Swing/Java2D shadow runtime has returned" }
        check("\"desktop\"" !in file("settings.gradle.kts").readText()) { "The retired desktop Gradle module is included" }
    }
}

tasks.named("check") {
    dependsOn(checkNative)
    dependsOn(checkReleaseNativeDependencies)
    dependsOn(checkInstalledNativeSmoke)
    dependsOn("checkDemoDsl")
    dependsOn("checkStrataAuthoring")
    dependsOn("validateStrataModules")
    dependsOn("checkBundledStrataArtifacts")
    dependsOn("checkNativeCoreCutover")
}
