# Strata UI Language for VS Code

The extension is generated and packaged from the same registry metadata used by the native compiler.
It provides:

- registry- and application-schema-aware completions for widgets, parameters, actions, host roots,
  properties, helpers, and local components/styles;
- hovers and signature help from generated type/dispatch metadata;
- document symbols and local/import definition navigation;
- exact source-ranged diagnostics from the production `strata_compile` executable;
- automatic adjacent `*.schemas.json` discovery plus explicit workspace settings.

## Native diagnostics

Install the Strata SDK or build the repository, then set `strata.compiler.path` if automatic
discovery cannot find `strata_compile`. `strata.registry.path` and
`strata.applicationSchemas.path` accept absolute paths, workspace-relative paths, or
`${workspaceFolder}` placeholders. The command **Strata: Validate Current Module** saves and checks
the active module immediately.

Validation intentionally runs on open/save rather than against a second JavaScript parser. The
native compiler remains the only authority for imports, schema composition, semantic types, and
source ranges.

## Repository build

CMake generates completion/grammar data and the VSIX package:

```sh
cmake --build --preset linux-x64 --target strata_generate_authoring
cmake --build --preset linux-x64 --target strata_vscode_extension
```

The package is written below the preset build tree. CTest checks generated metadata, JavaScript
syntax when Node.js is available, package contents, and machine-readable compiler diagnostics.
