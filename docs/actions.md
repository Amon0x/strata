# Actions: `.strata` intent to host effects

The [generated action reference](generated/strata-reference.md#declarative-actions) is authoritative
for built-in and registered action ids, payload fields, defaults, and dispatch policy.

An action is typed intent attached to a UI event. The event keeps its concrete value (text,
selection, drag phase, focus reason, and so on); the action adds a stable capability id, static
payload, policy, source key, and diagnostics metadata.

## Keep presentation operations native

Local state, focus, forms, named layers, overlays, collections, notifications, palette state, and
environment preferences use framework actions and do not cross the ABI or call a host:

```strata
state page = "HOME";
state modalOpen = false;

Tabs(selectedId: page, onChange: action("state.setFromEvent", name: "page"))
Button(label: "Open", onClick: action("state.set", name: "modalOpen", value: true))
Button(label: "Focus search", onClick: action("focus.request", key: "search"))
Button(label: "Validate", onClick: action("form.validate", key: "settings.form"))
Button(label: "Next screen", onClick: action("layer.push", name: "details"))
```

Use a host action only for a domain boundary: persistence, network requests, game mutation,
platform integration, external tools, or an engine probe.

## Declare the contract once

Application schema JSON declares every host action and its typed arguments. For example, F6's
schema declares `settings.save` and the source invokes it with the current form values:

```strata
Form(
  key: "settings.form",
  onSubmit: action("settings.save", displayName: displayName, telemetry: telemetry)
) { /* fields */ }
```

The native compiler resolves that id against the composed registry/schema, checks required and
optional fields, validates types, and retains the source range and component path. Unknown ids,
misspellings, or incompatible payloads reject activation and leave the last-good unit active.

## Register and own handlers

C++ applications register typed handlers through `strata::host::Bindings`:

```cpp
host.on("project.rename", [&](const strata::host::ActionEvent& event) {
    project.rename(event.payload.require_string("id"), event.value.require_string("name"));
    project_revision.changed();
    return strata::host::ActionResult::handled;
});
```

`ActionEvent` owns its action id, structured payload, event kind, optional stable source key, and
structured concrete value. Shared lifecycle projections such as `DragEvent` decode once in the
framework-facing layer rather than being reparsed by every application model.

C hosts register handlers per runtime through `strata_runtime_register_action_handler`. A
registration has an explicit release handle and owner label. Its callback receives borrowed
canonical JSON for the same fields and returns handled, forwarded, or ignored. Dispatch aggregates
handlers into handled, forwarded, ignored, unhandled, or failed outcomes. Exceptions from a C++
handler are contained inside the callback and rethrown by the next `Bindings::synchronize()` call.

The C ABI does not require hosts to duplicate presentation action handlers. Runtime-owned framework
actions execute inside the same native state/focus/layer/form systems that render the result.

## Dynamic escape hatch

Ordinary authored actions are statically registered. A dynamic action path exists only for explicit
scripting/remote-protocol cases and remains marked dynamic in dispatch output. It does not bypass
unknown/unhandled diagnostics or payload validation at the receiving boundary.

## Change boundary

- `.strata`/resource-only change: composition, styles, layout, motion, local state, validation,
  navigation, overlays, focus/forms, or invoking an existing capability.
- Schema and host rebuild: a new domain capability, changed payload contract, new host snapshot
  field, or new native widget/behavior package.

This boundary keeps source reload useful while making actual host/API changes explicit.
