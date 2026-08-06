# Authoring Strata UI

Strata has three API levels. Start in `.strata`, add typed host data/effects only where the
application crosses its UI boundary, and use native C++ extensions only for genuinely new widget or
behavior mechanics.

The [language guide](strata-language.md) describes syntax. The
[generated reference](generated/strata-reference.md) is projected from the authoritative native
built-in catalog:
widgets, properties, styles, layout, actions, motion, environment fields, materials, and effects.

## 1. Ordinary UI belongs in `.strata`

Screens, overlays, components/slots, design systems, navigation, forms, scrolling, local state,
derived collections, adaptive branches, semantics, and motion do not require host code.

```strata
style Transparent { background: null; border: null; }
style Card extends Transparent {
  kind: "COLUMN"; width: { weight: 1 }; height: "content";
  padding: 12; gap: 8; background: theme.surfaceRaised;
}

component SettingsCard(title: string) {
  defaults: { Button: { variant: "compact" } }
  Panel(style: Card) {
    Slot(name: "header") { Text(text: title) }
    Slot(name: "body", required: true)
    Slot(name: "footer")
  }
}

overlay Settings {
  state advanced = false;
  state sections: List<string> = ["account", "privacy"];
  derived summary = join(sections, " · ");

  root SettingsCard(title: "Display") {
    Slot(name: "body") {
      Toggle(label: "Advanced", bind: advanced)
      for section, index in sections where !isEmpty(section) {
        Text(key: section, text: format("{0}. {1}", index + 1, title(section)))
      }
    }
    Slot(name: "footer") { Text(text: summary) }
  }
}
```

Named slot content evaluates in the caller's scope; component parameters and state stay
encapsulated. Dynamic children use model-provided stable keys. Static leaves do not need ceremonial
keys.

Application-schema `types.definitions` make host record and bounded-list shapes directly nameable
in component parameters. Use those names instead of weakening reusable components to `any`.
`Binding<T>` is the explicit mutable parameter form for forwarding caller-owned retained state into
an authored control component.

`Section` owns a distinct disclosure header and padded, vertically spaced content region. Its
children stretch across that content region by default; use child `alignSelf`/`width` when a compact
control is intentional. `headerHeight`, `contentPadding`, and `contentGap` tune those regions without
replacing the Section's outer layout or interfering with header hit testing.

### Collections and virtualization

Derive bounded presentation data in the language:

```strata
state query = "";
derived matches = filter(projects.rows, row -> contains(lower(row.name), lower(trim(query))));

TextBox(bind: query, hint: "Search")
Text(text: format("{0} of {1}", matches.matched, matches.total))
Repeater(key: "project.results", estimatedItemExtent: 44) {
  for row in matches { ProjectRow(key: row.key, row: row) }
}
```

`filter`, `map`, `sortBy`, `distinctBy`, `groupBy`, `flatten`, `takeWhile`, `window`, and `page`
return immutable typed views. They are memoized by source generation and captured values and expose
inspection metadata. A large eager loop is rejected; `Repeater` materializes only the visible range
plus overscan and retains its stable-key anchor through reordering/filtering.

Use local structured state for bounded presentation data. Move data to a host snapshot when it must
be persisted, shared outside the application, loaded asynchronously by the host, or synchronized
with engine/game state.

### Styles, themes, and motion

Use named styles and `theme.*` tokens for a design system. Explicit call-site properties win over
component defaults and styles. Control skins are still ordinary built-ins; changing toggle track/
thumb geometry or checkbox indicators does not justify another widget implementation.

`Checkbox.presentationTemplate` separates its input contract from its authored skin. The native
control continues to own checked state, pointer and keyboard activation, focus, and semantics while
the template receives `key`, `label`, `description`, and one typed `checkboxState` record. Read
`control.checked`, `control.enabled`, `control.hovered`, `control.pressed`, `control.focused`, and
`control.focusVisible` inside the template rather than reimplementing a checkbox with Panel state.

Editable controls separate mechanics from optional chrome. Use `appearance: "BARE"` on `TextBox`,
`TextArea`, or `NumberField` when an authored component owns the surrounding surface:

```strata
Panel(style: SearchGlass) {
  TextBox(key: "search.editor", bind: query, appearance: "BARE", hint: "Search…")
}
```

The editor keeps focus, selection, caret, IME, undo, semantics, and input routing while suppressing
its native background, border, focus ring, and interaction overlays. This is preferable to encoding
presentation suppression through nullable paint fields.

### Anchored overlays and authored controls

`PORTAL` layout is out of parent flow and can be anchored to any stable widget key. `anchorSide`,
`anchorAlign`, `anchorGap`, `anchorFlip`, `anchorShift`, and `matchAnchorWidth` control placement;
`portalTarget` and `detachFromParentClip` control the detached render root. Flip chooses the side
with more room and shift keeps the result inside the root viewport. `anchorPoint: { x, y }`
replaces the target rectangle for pointer-positioned overlays.

`Menu` and `Select` accept typed `triggerTemplate`, `popupTemplate`, and `itemTemplate` components.
The trigger can be replaced independently; supply `popupTemplate` and `itemTemplate` together.
The popup component is the authored surface/container; Strata injects
the materialized item components as its children and places it through an anchored portal. The
typed item context includes stable key/id/value/index, label, enabled, selected/active/checked
state, separator status, nesting level, shortcut, and child disclosure. Template instances are
visual presentation owned by the parent control: nested focusable or clickable widgets do not
create competing interaction targets. `ComboBox` forwards the same templates to its choice popup.
`Tooltip.contentTemplate` authors its detached content.

The application therefore owns rows, icons, badges, padding, borders, effects, and motion. Native
code still owns open state, outside dismissal, focus, pointer routing, keyboard navigation,
typeahead, selection/action dispatch, accessibility, and viewport collision:

```strata
component ChoicePopup(key: key, level: number, expanded: boolean) {
  Panel(key: key, style: GlassPopup) // injected item children are appended here
}

Select(
  key: "quality",
  options: [{ id: "fast", label: "Fast" }, { id: "best", label: "Best" }],
  triggerTemplate: ChoiceTrigger,
  popupTemplate: ChoicePopup,
  itemTemplate: ChoiceItem
)
```

See `showcase_components.strata` for a complete optic-glass Menu, Select, ComboBox, and Tooltip
skin built from the same mechanism.

Named timelines attach through `transition`, `enter`/`exit`, `move`, `contentTransition`, or motion
channels. Boolean/numeric/color retargeting interrupts from the displayed value. Content-size and
disclosure motion use the same retained layout/focus/input system. Reduced-motion surfaces resolve
to the same final tree without a parallel behavior implementation.

### Typography rasterization

Text uses the bundled Regular face and size-specific, TrueType-hinted grayscale masks by default at
every font size and display scale. Controls use the Medium face; headings and other emphasized text
should select `strata:fonts/default-medium` explicitly. Grayscale runs are regenerated
for their physical scale, retain quarter-pixel horizontal positioning, and use R8 atlas pages.

MSDF remains available for text that is deliberately scaled or transformed after layout. Opt in on
only those runs:

```strata
style ScalableTitle {
  pixelSize: 32;
  fontRasterization: "MSDF";
}
```

`fontRasterization` accepts `"GRAYSCALE"` and `"MSDF"`; rich-text spans may override it through
their span style. Merely being large does not implicitly select MSDF.

### Semantics and actions

Built-ins derive normal roles, names, state, and actions. Use bounded `.strata` semantic overrides
only where a custom composition has a derivation gap; overrides cannot invent executable input
capability.

Presentation actions remain in the runtime:

```strata
Button(label: "Focus search", onClick: action("focus.request", key: "search"))
Button(label: "Validate", onClick: action("form.validate", key: "settings.form"))
Button(label: "Open details", onClick: action("layer.push", name: "details"))
```

Use a host action for persistence, networking, game state, platform integration, or another domain
effect. See [Actions](actions.md).

## 2. Typed host bindings and effects

Application schema JSON declares host object paths and registered action payloads. The native
compiler rejects missing required fields, wrong types, unknown actions, invalid component slots, and
unbounded host iteration before activation.

Publish host data as immutable canonical JSON with a monotonically increasing generation. Register
action callbacks on the owning runtime. Callback payload/event JSON is borrowed only for that call;
copy it if the host retains it. A handler returns handled/forwarded/ignored, and dispatch records
handled/unhandled/failed outcomes for diagnostics and canonical frames.

The copyable examples are:

- `assets/strata/ui/settings_app.schemas.json` and `settings_app.strata`;
- `demo_surface.schemas.json`, `demo_surface.strata`, and `showcase_components.strata`;
- `debug_overlay.schemas.json` and `debug_overlay.strata`.

The desktop and headless application hosts demonstrate the boundary: construct snapshot JSON and
handle domain effects while compilation, state, retained UI, layout, input, semantics, and render
planning remain in the runtime.

Render effects are also application declarations, not native widgets. A typed ordered effect
program can filter the already-rendered backdrop or isolate and filter a component subtree. D3D11
executes blur and authored HLSL passes; the software host runs the declared blur subset and applies
the same bounds, rounded mask, opacity, and content composition deterministically. See
[the language guide](strata-language.md#authored-render-effects).

## 3. Native extensions

Only use a registered native widget/behavior when the interaction or rendering contract is actually
new: for example continuous thumb dragging, a distinct semantic control, custom simulation, or a
new primitive/material path.

Extensions are registered per Surface through the public ABI. Their package/schema declarations
travel through application schema composition. They receive bounded native lifecycle,
input, measure/arrange, semantics, and render callbacks; they do not acquire compiler internals or
host-global state. Built-ins and extensions share stable identity, invalidation, focus/capture,
motion, layout, semantics, and packet planning.

Adding a reusable custom composition is not an extension reason—write a `.strata` component. Adding
a game callback is not an extension reason—register a typed host action.

## Diagnostics and live editing

Diagnostics carry stable codes, exact source ranges, component paths, expected types/names, and
occurrence counts. The full catalog is [generated/diagnostics.md](generated/diagnostics.md).

Activation is last-good. Rejected source never replaces the active unit. A host resource reload can
read new source and call activation on the same runtime/Surface, preserving compatible stable-key
state. Fonts and PNG/SVG images are also candidate-loaded before adoption; rejected resources retain the
previous usable set.

Regenerate catalog-derived authoring files explicitly:

```sh
cmake --build --preset linux-x64 --target strata_generate_authoring
```

Use the `windows-x64` preset on Windows. The ordinary CTest gate runs the equivalent
`strata_check_authoring` check and fails if reference, diagnostic, completion, grammar, generated
C++ host-contract, generated registry projection, or lexical artifacts are stale. External projects invoke the installed
`Strata_AUTHORING` tool directly for their own application schema.
