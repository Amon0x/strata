# `.strata` language syntax

This page describes the language itself. The generated
[widget/action/property reference](generated/strata-reference.md) is the authoritative list of
registered names, types, defaults, enum values, events, and retained state. The
[diagnostics catalog](generated/diagnostics.md) gives hand-authored cause and recovery guidance for
every diagnostic code present in production source; the build verifies exact coverage.

## Files, imports, and comments

A module is a UTF-8 `.strata` file. Imports use a quoted path resolved relative to the importing
file. They may reach any module under the application's resource root, so apps can share
components from a sibling folder (`import "../shared/forms.strata";`), and never outside it.
`strata_compile --check-module` takes the same root with `--resource-root <directory>` (the VS Code
setting `strata.resourceRoot.path`); without one it keeps imports inside the entry module's folder.

```strata
import "components/forms.strata";

// line comment
/* block comment */
```

Imports form one compiled module. Duplicate declarations and import cycles are errors. Editing any
dependency recompiles the module atomically; a rejected edit leaves the last-good unit active.

A module with syntax errors, or an import that cannot be loaded, is still checked, so every problem
shows at once. An unknown name is left unreported only when a missing declaration could explain
it: a name written inside a declaration or statement the parser had to skip, any name once a module
stops at a compiler limit, or a component, style, animation, or state name while an import is
missing. Other misspellings, such as a local value read under the wrong name, are still errors.

## Lexical forms

- Identifiers begin with `_` or a Unicode letter and continue with letters, digits, or `_`.
- Strings use double quotes and backslash escapes.
- Numbers support `_` separators, fractions, exponents, and `ms`/`s` duration suffixes.
- Colors use `#RGB`, `#RGBA`, `#RRGGBB`, or `#RRGGBBAA`.
- Literals are strings, numbers/durations, colors, `true`, `false`, `null`, lists, and objects.
- Statements and style properties may end with `;`; the separator is optional, so a line break
  is enough. Map entries are separated by `,` (or `;`).

The VS Code extension under `editor/vscode` packages the generated TextMate grammar and catalog
metadata. It provides schema-aware widget/action/host/property completions, hovers, signature help,
symbols, import/local-definition navigation, and exact diagnostics from `strata_compile
--check-module-json`. It does not maintain a second JavaScript parser: the production compiler
remains the validation authority. Build `strata_vscode_extension` to produce the installable VSIX.

## Screens and overlays

Screens and overlays are named entry points. Exactly one `root` widget supplies their root node.

```strata
screen Main {
  root Panel(layout: { kind: "COLUMN", width: { weight: 1 }, height: { weight: 1 } }) {
    Text(text: "Hello")
  }
}

overlay Dialogs {
  root Modal(open: true) { Text(text: "Saved") }
}
```

The host registers which entry points correspond to named screen/layer operations.

## Components, parameters, defaults, and slots

Components encapsulate parameters and local state. Parameters may carry a semantic type and a
default. A parameter without a default is required. Components can set widget defaults for their
subtree; an explicit argument at a widget call always wins.

```strata
component Card(title: string, compact: boolean = false) {
  defaults: {
    Button: { variant: "compact", style: CardButton },
    Text: { style: CardText }
  }
  Panel(variant: compact ? "compact" : "default") {
    Slot(name: "header") { Text(text: title) }
    Slot(name: "content", required: true)
    Slot(name: "footer") { Text(text: "Default footer") }
  }
}

overlay Demo {
  root Card(title: "Account") {
    Slot(name: "content") { Text(text: "Caller-owned content") }
    Slot(name: "footer") { Button(label: "Save") }
  }
}
```

Slot names and occupancy are checked statically. A slot body is its default when the caller omits
that slot; `required: true` rejects omission. Unnamed caller children remain shorthand when the
component has exactly one slot (or a slot named `content`). Filled expressions keep the caller's
scope and stable identity; component parameters and state remain private.

A component template handed to a widget's row-template parameter (a `Table`'s `rowTemplate`,
`cellTemplate` and `cellTemplates`, the `itemTemplate` of a `VirtualList` or `ItemGrid`, a
`TreeView`'s `rowTemplate`) describes each generated row inside the component that hands it over.
The slots it declares, and those of the row templates it hands on in turn, are that component's:
its callers fill them, and every row shows the content.

Application schemas may declare reusable semantic types for component parameters and host roots:

```json
"types": {
  "definitions": [
    {
      "id": "ProjectRow",
      "type": {
        "kind": "object",
        "label": "project row",
        "allowUnknownFields": false,
        "fields": [
          { "name": "key", "type": { "kind": "key" }, "required": true, "nullable": false },
          { "name": "name", "type": { "kind": "string" }, "required": true, "nullable": false }
        ]
      }
    },
    {
      "id": "ProjectRows",
      "type": {
        "kind": "list",
        "minimumItems": 0,
        "maximumItems": 5000,
        "elementNullable": false,
        "element": { "ref": "ProjectRow" }
      }
    }
  ]
}
```

`component ProjectList(rows: ProjectRows)` preserves both the record fields and the declared
collection bound through the component boundary. Type references are available to every module in
that application and may also be reused from host/action schemas with `{ "ref": "ProjectRow" }`.
Duplicate and cyclic application types reject schema composition.

Host fields declared as `color` use JSON `[red, green, blue, alpha]`, with four integer channels
in `0..255`. Snapshot conversion produces a color value, including inside lists and objects, so
expressions can pass the field directly to a shape's `fill` or `stroke`. Arrays without a color
schema remain lists.

Use `Binding<T>` when a reusable component must edit caller-owned retained state:

```strata
component SearchField(query: Binding<string>) {
  TextBox(key: "search.editor", bind: query, hint: "Search")
}

component SearchPage() {
  state query = "";
  SearchField(query: query)
}
```

A `Binding<T>` argument must be retained state or another compatible `Binding<T>` parameter.
Temporary values, derived values, host fields, and defaults are rejected. The binding retains its
original state address through nested component forwarding, so editor undo and declarative state
actions still mutate the caller's state.

Component templates are values and may bind additional parameters before they are passed to a
framework owner. The remaining signature is checked structurally against the owner's contract:

```strata
component MeterPresentation(
  key: key,
  suffix: string,
  control: progressState
) {
  Text(key: key, text: format("{0}{1}", control.value, suffix))
}

Progress(
  key: "upload.progress",
  value: uploaded,
  presentationTemplate: MeterPresentation(suffix: "%")
)
```

The owner still supplies `key` and `control`; pre-binding an owner-supplied parameter is a compile
error. Bound actions and other executable values retain the caller's lexical scope when the
template is forwarded through another component.

## Styles and theme values

Styles compose left to right. A derived style overrides its bases in declaration order; inline
composition applies named styles first and named overrides last. Unknown bases and cycles are
compile errors. `theme.*` values remain symbolic until the surface resolves its effective theme.
When `clip: true` is set, descendants are clipped to the same authored `radius` as the container;
the rounded clip follows presentation transforms rather than degrading to its axis-aligned bounds.
`radius` is a number for every corner or a map naming corners (`topLeft`, `topRight`,
`bottomRight`, `bottomLeft`) or sides (`top`, `bottom`, `left`, `right`), most specific first:
`radius: { top: 12, bottomRight: 4 }` rounds a tab's top corners and one bottom corner. Fills,
borders, shadows and clips all follow the per-corner shape.

Text takes `fontWeight` (a number from 1 to 1000, or `THIN` 100, `LIGHT` 300, `NORMAL` 400,
`MEDIUM` 500, `SEMIBOLD` 600, `BOLD` 700 or `BLACK` 900) and `fontStyle` (`NORMAL` or `ITALIC`),
on the node or on a rich text span's style. Every font the surface binds that shares the requested
font's family competes for the text: an italic face for italic, then the face whose OS/2 weight is
nearest, so binding a family's faces is all it takes to use them. Only when no bound face is bold
or italic is the glyph emboldened or slanted; a face of weight 600 or more counts as bold. `textDecoration` draws `UNDERLINE`, `LINE_THROUGH` or both
(`UNDERLINE_LINE_THROUGH`) under each line the text covers, in the text's colour unless
`textDecorationColor` sets one; a span's `textDecoration: "NONE"` turns its node's decoration off.

```strata
style Transparent { background: null; border: null; }
style Spaced { padding: 12; gap: 8; }
style CardStyle extends Transparent, Spaced {
  kind: "COLUMN";
  width: { weight: 1 };
  height: "content";
  radius: 6;
  background: theme.surfaceRaised;
}

Panel(style: style(CardStyle, padding: 8))
```

Conditional style layers use any Boolean state rather than a fixed set of pseudo-states. Layers
whose conditions are false contribute nothing, and later active layers win:

```strata
Panel(style: style(
  Thumb,
  whenStyle(hovered || focusVisible, ThumbHover),
  whenStyle(pressed, ThumbPressed)
))
```

The native `rail` slider presentation keeps high-frequency dragging inside one retained render
fragment while leaving its geometry authored. `trackGap` and `activeTrackGap` control the resting
and pressed space around the thumb; `thumbWidth`/`thumbHeight` and their `activeThumbWidth`/
`activeThumbHeight` counterparts control the two thumb states. Omitted active values inherit their
resting value, so a press never changes track or thumb geometry unless the style requests it.

The generated reference separates general style properties, layout properties/size forms, text
properties, and enum values. `theme.<name>` reads three kinds of token, and the compiler checks
each token's kind where it is used:

| Token | Kind |
|---|---|
| `theme.surface`, `theme.surfaceRaised` | color |
| `theme.foreground`, `theme.mutedForeground` | color |
| `theme.accent`, `theme.danger`, `theme.focus` | color |
| `theme.spacingUnit`, `theme.radius`, `theme.density` | number |
| Code: `theme.codeBackground`, `theme.codeLineNumber`, `theme.codeLineHighlight` and the syntax colours `theme.syntaxKeyword`, `theme.syntaxType`, `theme.syntaxBuiltin`, `theme.syntaxFunction`, `theme.syntaxString`, `theme.syntaxNumber`, `theme.syntaxComment`, `theme.syntaxOperator`, `theme.syntaxPunctuation`, `theme.syntaxConstant`, `theme.syntaxProperty`, `theme.syntaxTag`, `theme.syntaxAttribute`, `theme.syntaxVariable`, `theme.syntaxHeading`, `theme.syntaxEmphasis`, `theme.syntaxLink`, `theme.syntaxMeta` | color |
| Palette: `theme.popupSurface`, `theme.tooltipSurface`, `theme.paletteSurface`, `theme.link`, `theme.disabledForeground`, `theme.matchHighlight`, `theme.insertionIndicator`, `theme.shadow`, `theme.severityError`, `theme.severityWarning`, `theme.severityInfo`, `theme.severitySuccess`, `theme.tokenFill`, `theme.tokenActive`, `theme.tokenActiveBorder`, `theme.marqueeFill`, `theme.marqueeStroke`, `theme.compositionSelection`, `theme.compositionUnderline`, `theme.tableRowDivider`, `theme.tableColumnDivider`, `theme.tablePinnedDivider` | color |
| Custom tokens the application schemas declare | as declared |

Palette tokens are the colors built-in widgets draw with. Every theme resolves them: a theme that
does not set one gets its light or dark default, chosen by the brightness of its `surface`. A theme
may also define any other token by name (see [Themes](embedding.md#themes)); code reads one only
after the application schemas declare it with its kind:

```json
"themeTokens": {
  "definitions": [
    { "name": "brandAccent", "type": "color" },
    { "name": "gutter", "type": "number" }
  ]
}
```

A name that is none of these is a compile error (`STRATA.DSL.SEMANTIC_UNKNOWN_THEME_TOKEN`, with a
suggestion for a near miss) rather than a value that resolves to nothing at runtime. Themes are
registered by the host at runtime, after compilation, so the compiler cannot see which custom tokens
they will define; without a declaration, a typo such as `theme.acent` would compile and silently
leave the property unstyled, and nothing would say whether the token is a color or a number. The
declaration states that contract once. A declared custom token that the effective theme does not
define leaves the property at its themed default, as an unset style property would.

A token stays symbolic while the surface evaluates expressions and resolves when it applies the
style property against the effective theme of that node, so a `ThemeScope` subtree reads its own
values. A token resolves wherever it sits: a style property, a field inside one (a border's
`color`, a shadow in `shadows`, a gradient stop), or a widget argument such as an image's `tint`.
Operators cannot compute with a token (`theme.spacingUnit * 2` is a type error); write the token
itself as the value.

### Layer placement and anchors

Children of layered layouts (`PANEL`, `OVERLAY`, and `STACK`) can position either axis against the
parent content box. Numeric positions are logical pixels; `{ fraction: value }` positions are a
fraction of the available axis. `anchorX`/`anchorY` select the corresponding point on the child,
and scalar offsets are applied last:

```strata
Panel(layout: { kind: "PANEL", width: 240, height: 40 }) {
  Panel(layout: {
    width: 8,
    height: 24,
    placement: {
      x: { fraction: progress },
      anchorX: 0.5,
      offsetY: 2
    }
  })
}
```

Percentage sizes use the same `{ fraction: expression }` arithmetic. Fill allocation is
proportional to its authored weight; an explicit `{ weight: 0 }` receives zero remaining space.

`anchorTarget` is also valid on an ordinary child. The child becomes out-of-flow, is measured
against its containing content box, and is arranged after its keyed sibling target, so chained
sibling anchors are deterministic. Ordinary keyed targets must share the same parent and must not
be portals; use `"parent"` for the containing node.

```strata
Panel(key: "account.trigger", layout: { width: 120, height: 28 })
Panel(layout: {
  anchorTarget: "account.trigger",
  anchorSide: "BOTTOM",
  anchorAlign: "END",
  anchorGap: 6
})
```

Missing targets and cyclic chains produce layout diagnostics instead of order-dependent geometry.

### Absolute and sticky positioning

`position: "ABSOLUTE"` takes a child out of its parent's flow in any container: a `ROW`, `COLUMN`,
`GRID` or `SCROLL` lays out its other children as if it were not there. `inset` places it against
the parent's padding box: a number insets every edge, and a map names the edges it sets (`left`,
`top`, `right`, `bottom`, or `horizontal`, `vertical` and `all`). An absolute child with both insets
on an axis and an automatic size stretches between them; one inset pins that edge; none places it
at the start. `placement` still applies on top, and a `{ fraction }` size is a fraction of the
padding box. Inside a `SCROLL`, an absolute child stays put over the scrolling content.

```strata
Panel(layout: { kind: "ROW", gap: 8 }) {
  Button(label: "Inbox")
  // A badge on the corner, outside the row's flow.
  Panel(style: style(position: "ABSOLUTE", inset: { top: -4, right: -4 },
                     width: 16, height: 16, radius: 8, background: theme.danger))
}
```

`position: "STICKY"` keeps a child in flow until scrolling would carry one of its inset edges past
the nearest scroll viewport; it then holds at that inset, but never leaves the box its parent lays
it out in, so a section's sticky header leaves with its section and the next one takes over.

```strata
Scroll(layout: { kind: "SCROLL", height: { weight: 1 } }) {
  for group in groups {
    Panel(key: group.id, layout: { kind: "COLUMN" }) {
      Text(text: group.title, layout: { position: "STICKY", inset: { top: 0 } })
      for row in group.rows { Row(key: row.id, row: row) }
    }
  }
}
```

Absolute and sticky children paint and are hit above in-flow siblings with the same `zIndex`.

### Responsive rows and children

A `ROW` with `stackBelow: n` lays its children out as a `COLUMN` while the content width it is
given is under `n` logical pixels. A child with `hideBelow: n` or `hideAbove: n` leaves layout
(as `participates: false` would) while its parent's content width is under or over `n`. Both
resolve during layout against the space actually given, so a sidebar collapsing or a split pane
resizing reflows them without rebuilding the description:

```strata
Panel(layout: { kind: "ROW", gap: 12, stackBelow: 520 }) {
  Avatar(user: user)
  Panel(layout: { kind: "COLUMN", width: { weight: 1 } }) { Details(user: user) }
  Panel(layout: { hideBelow: 520 }) { Actions(user: user) }
}
```

Two alternatives with complementary thresholds swap whole subtrees by width: both are described,
and only the one that fits takes part in layout, hit testing and drawing.

```strata
Panel(layout: { kind: "PANEL", width: { weight: 1 } }) {
  CompactToolbar(layout: { hideAbove: 600 })
  FullToolbar(layout: { hideBelow: 600 })
}
```

A `SCROLL` layout's `scrollSnap` (`START`, `CENTER` or `END`) settles it on a child once the user
stops scrolling: about 120 ms after the last wheel step or drag, and not while a press is held, it
eases to the nearest child's start, centre or end. Its snap points are its children, or the
children of its one content box, so a carousel or a picker needs no extra structure.

An app scrolls a view itself with `scroll.to`, as `element.scrollTo` does on the web: the keyed
view's `x` and/or `y` offset, clamped to its range. Without a `timing` it jumps there; with one (a
theme motion timing, optionally a `duration` in milliseconds) it eases there the way a scroll
animation does, and with reduced motion it jumps. To bring a keyed node into view instead, use
`reveal.request`.

```strata
Button(label: "Back to top", onClick: action("scroll.to", key: "feed", y: 0, timing: "standard"))
```

### Masonry and canvas layouts

`kind: "MASONRY"` lays its children out in equal columns, Pinterest-style: each child is as wide as
a column, as tall as its content (or its own height) makes it, and goes into the column that is
shortest when its turn comes (the leftmost of equals). `columns` sets the column count;
`minColumnWidth` derives it from the width instead, as many columns at least that wide as fit (with
both, `columns` is the most it takes; with neither, columns are at least 240 wide). `gap` spaces the
columns and the children in them. The box is as tall as its tallest column, so a masonry wall in a
`SCROLL` scrolls its full height; placement is one pass over the children.

```strata
Scroll(vertical: true) {
  Panel(layout: { kind: "MASONRY", width: { weight: 1 }, minColumnWidth: 180, gap: 10 }) {
    for photo in photos {
      Panel(key: photo.id, style: style(Card, height: photo.height)) { Text(text: photo.title) }
    }
  }
}
```

`kind: "CANVAS"` lays each child out at its own natural size, unbounded by the canvas's size, at
its `inset` `left` and `top` from the canvas's content origin (0 when unset). It suits boards and
node graphs whose children have coordinates of their own; the canvas is as large as its own size
says, or as far as its furthest child reaches. `ZoomView` lays its content out this way.

```strata
Panel(layout: { kind: "CANVAS", width: 1400, height: 900 }) {
  Node(title: "Source", layout: { inset: { left: 120, top: 110 } })
  Node(title: "Mix", layout: { inset: { left: 560, top: 360 } })
}
```

### Zoomable canvases

`ZoomView` presents its one child, laid out at its natural size, under a pan and zoom the user
controls: the wheel zooms about the cursor (eased, by 1.2 a notch), dragging the background or
content that does not take the pointer pans, a middle-button drag pans from anywhere, and Shift with
the wheel (or a sideways swipe) pans. Pressing the view focuses it; then `+` or `=` zooms in and
`-` out about its middle (with or without Ctrl, and from a focused control on the canvas too), `0`
goes back to the content's own size, Shift+1 fits all of it, and the arrows pan (Shift: five times
as far). `minZoom` and `maxZoom` bound it (0.1 and 8 by default).
Everything inside stays live: presses, hover and hit testing follow the zoomed content, so buttons
on a zoomed canvas click. While a zoom moves, the content is resampled from what was last drawn;
once it rests, text and shapes are rasterized again at exactly the zoom they show, so they stay
sharp rather than magnified. A zoom that moves an octave or more before resting steps its
rasterization on the way.

`view` (`{x, y, zoom}`: where the content's top-left shows, relative to where it is laid out, and
its scale) with `onViewChange`, or `bind` to a state, lets the app read and restore it; a `view` the
app sets is eased to. The app hears of the view once it comes to rest (a pan released, an eased
zoom arrived), not on every frame of the gesture; `liveView: true` reports every frame, for an app
that draws something following the view as it moves. `zoom.fit {key, padding}` eases to show all of the content (padding 24 by
default) and `zoom.reset {key}` back to its own size; with no key they act on the first ZoomView.
`persistenceKey` keeps an uncontrolled view across runs. `panButton` chooses what drags the view:
`"primary"` (the default) on the canvas itself or content that does not take the pointer;
`"secondary"` from anywhere, leaving a primary drag on the canvas to the content (a marquee
selection in a node editor); `"middle"` alone. The middle button always pans.

```strata
state view = { x: 0, y: 0, zoom: 1 };
Button(label: "Fit", onClick: action("zoom.fit", key: "board", padding: 32))
Text(text: format("{0}%", round(view.zoom * 100)))
ZoomView(key: "board", bind: view, minZoom: 0.25, maxZoom: 4, layout: { height: 480 }) {
  Panel(layout: { kind: "CANVAS", width: 1400, height: 900 }) { Board() }
}
```

### Image fit

`Image(fit:)` places an image of known size in its bounds: `FILL` (the default) stretches it;
`CONTAIN` shows all of it at its aspect ratio; `COVER` fills the bounds at its aspect ratio and
crops what overflows; `NONE` draws it at its natural size; `SCALE_DOWN` is `NONE` until the image is
larger than its bounds, then `CONTAIN`. `alignX` and `alignY` (`START`, `CENTER`, `END`) place what
does not fill and choose which part a crop keeps. The natural size comes from the SVG or raster
image (PNG, JPEG, GIF or WebP) the host bound, or from a runtime image the host published.

```strata
Image(image: user.avatar, fit: "COVER", style: style(width: 40, height: 40, radius: 20, clip: true))
```

### Calendar

`Calendar` shows a month and picks a day, exchanged as `"YYYY-MM-DD"` text: `value` (or `bind`) with
`onChange`, `defaultValue` for an uncontrolled calendar, `min` and `max` to bound what can be
picked, `today` to ring the current day (the host knows the date; pass it in), and
`firstDayOfWeek` (0 for Sunday, 1 for Monday, the default). Pointer users pick a day or step months
with the header buttons; keyboard users move by day with the arrows, by month with Page Up and Page
Down (a year with Shift), to the week's ends with Home and End, and pick with Enter or Space.

`selection: "RANGE"` picks a span instead: the first pick starts it, the next ends it (in either
order), and a pick after that starts a new one. `rangeStart`/`rangeEnd` control it, and
`onRangeChange` carries `{ start, end }`, with `end` null until it is picked; the days between are
banded. `monthNames` (twelve) and `weekdayNames` (seven, from Sunday) put the calendar in any
language.

```strata
Calendar(bind: due, today: env.today, min: env.today)
Calendar(selection: "RANGE", onRangeChange: action("state.setFromEvent", name: "trip"),
         monthNames: locale.months, weekdayNames: locale.weekdayInitials)
```

### Date picker

`DatePicker` is a field that opens a `Calendar` dropdown. It takes the same `value`/`bind`,
`defaultValue`, `min`, `max`, `today`, `firstDayOfWeek`, `monthNames` and `weekdayNames`, and
`onChange` carries the picked `"YYYY-MM-DD"`. The field shows the day like "Mar 15, 2024", or in
`format` when given (the `formatDate` pattern language), and `placeholder` while nothing is picked.
A click, or Enter, Space or Down, opens it; inside it, the Calendar's keys move and Enter picks.
Picking a day, Escape or a press outside closes it.

```strata
DatePicker(bind: due, today: env.today, min: env.today, format: "EEE d MMM yyyy")
```

### Range slider

`RangeSlider` picks a span between `min` and `max` with two thumbs: `start` and `end` control it,
`defaultStart` and `defaultEnd` start an uncontrolled one, and `onChange` carries `{ start, end }`.
`step` snaps both, and `minGap` keeps them that far apart. A press takes the nearer thumb and drags
it; the arrow keys step the keyboard's thumb (Page Up and Page Down by ten steps, Home and End to
the limits), and Tab moves from the start thumb to the end one before focus leaves (Shift+Tab
back). `showValue` floats each thumb's value above it while hovered or focused, with
`valueSuffix`.

```strata
RangeSlider(min: 0, max: 500, step: 10, minGap: 20, defaultStart: 100, defaultEnd: 400,
            showValue: true, valueSuffix: "$",
            onChange: action("state.setFromEvent", name: "price"))
```

### Color picker

`ColorPicker` picks a color from a saturation and value square and a hue strip, with an opacity
strip when `showAlpha` is set. `value`/`bind` and `onChange` exchange the color, `defaultValue`
starts an uncontrolled one, and `swatches` adds preset colors below it. Its hex value shows next to
a preview. Dragging adjusts whichever part the press landed in; Left and Right change saturation,
Up and Down value (by 1%, or 10% with Shift), and Page Up and Page Down turn the hue.

```strata
ColorPicker(bind: accent, showAlpha: true, swatches: [#EF4444, #22C55E, #3B82F6])
```

### Charts

`Chart` plots `series` (each `{ name, values, color? }`, a value per category, `null` for a gap)
against `labels`, or a single `values` list. `kind` is `LINE` (the default), `AREA`, `BAR`,
`SCATTER`, `PIE` or `DONUT`; a pie or donut slices the first series by `labels`. `stacked` stacks
bars and areas. The value axis picks round ticks for its height (`min` and `max` override its
range), labels them with `valueSuffix` and `decimals`, and draws gridlines unless `showGrid` is
false; category labels thin out rather than collide. `showPoints` marks each line point. Series
colors come from the theme palette's `chart1` to `chart8` unless a series names its own.

The legend (shown for more than one series, and for pies, unless `showLegend` says otherwise) hides
and shows a series, or a slice, when clicked. Hovering a category draws a crosshair and a tooltip of
each series' value; hovering a slice lifts it out, and a donut names it in its middle. Clicking, or
Enter after the arrow keys, selects a category or slice: `onSelect` carries
`{ index, label, value, values }`.

A chart animates its data with the theme's standard motion policy: when its values change, bars
grow and shrink, lines morph and slices sweep to their new shares, and a series or slice hidden
from the legend fades out (or sweeps closed) rather than vanishing. A change of structure (another
number of categories or series, or a gap appearing) draws the new data at once, as does reduced
motion. An authored `animateChanges` replaces the default; one without `"data"` in its
`properties` turns the animation off.

```strata
Chart(kind: "BAR", labels: ["Q1", "Q2", "Q3", "Q4"], stacked: true,
      series: [{ name: "Online", values: [42, 55, 61, 70] },
               { name: "Retail", values: [30, 28, 35, 33] }],
      onSelect: action("state.setFromEvent", name: "quarter"))
Chart(kind: "DONUT", labels: ["Chrome", "Safari", "Other"], values: [64, 19, 17], valueSuffix: "%")
```

### Markdown

`Markdown` renders `text` as rich content: headings, paragraphs (a line ending in two spaces or a
backslash breaks), `**bold**`, `*italic*`, `` `code` ``, `~~strikethrough~~`, links (`[text](url)`,
`<url>` and bare `https://` addresses), bulleted, numbered and task lists (nested by indentation),
quotes, fenced and indented code blocks, tables and rules. It takes its text style (font, size,
color) from its own style, uses `codeFont` (the bundled mono face by default) for code, and themes'
link color for links. Following a link runs `onLink` with its destination.

```strata
Markdown(text: release.notes, style: { pixelSize: 14 },
         onLink: action("state.setFromEvent", name: "followedLink"))
```

The links are ordinary `RichText` link spans. Any span can carry an `href`: the action its `action`
names receives the `href` as its event value. A fenced block names its language after the fence
(```` ```rust ````) and is drawn by a `CodeView` highlighting that language, without line numbers, so
the Markdown's code reads like the rest of the app's; a `syntax` in the Markdown's style reaches
its blocks.

### Code view

`CodeView` shows source code in a monospace face, highlighted by a real tokenizer for its
`language`: `json`, `javascript`, `typescript`, `python`, `c`, `cpp`, `csharp`, `rust`, `go`,
`css`, `html`, `xml`, `markdown`, `shell`, `sql` and `strata`, each also by its usual aliases and file
extensions (`js`, `tsx`, `py`, `c++`, `hpp`, `cs`, `rs`, `golang`, `scss`, `svg`, `md`, `bash`,
`postgres`, ...). Block comments, multi-line and template strings, an HTML page's script and style,
and a Markdown file's fenced blocks all carry across lines, because the whole text is tokenized at
once, and only again when it changes. An unknown language shows plain text.

```strata
CodeView(text: source, language: "rust", lineNumbers: true, wrap: false, highlightLines: [12, 13],
         style: style(height: 360, syntax: { keyword: #FF79C6FF, comment: theme.mutedForeground }))
```

`lineNumbers` (on by default) puts numbers in a gutter that stays at the left edge while the code
scrolls sideways under it; `firstLine` numbers from another line. `wrap` (off by default) breaks
long lines instead, each keeping its number beside its first row. `highlightLines` bands the lines
it lists. `tabSize` (4) expands tabs to columns. The text is selectable (`selectable: false` turns
that off): a drag selects across lines, scrolling the view (or the page around it) when it is
dragged past an edge, Shift+click and Shift with the arrow, Home and End keys extend a selection,
Ctrl+A selects all of it, and a copy gives back exactly the selected text, never the line numbers.
Its font is `strata:fonts/mono` at 13 pixels unless its style sets `font`,
`pixelSize`, `color` or `lineHeightMultiplier`.

The colours come from the theme's palette (`syntaxKeyword`, `syntaxType`, `syntaxBuiltin`,
`syntaxFunction`, `syntaxString`, `syntaxNumber`, `syntaxComment`, `syntaxOperator`,
`syntaxPunctuation`, `syntaxConstant`, `syntaxProperty`, `syntaxTag`, `syntaxAttribute`,
`syntaxVariable`, `syntaxHeading`, `syntaxEmphasis`, `syntaxLink`, `syntaxMeta`, and
`codeBackground`, `codeLineNumber` and `codeLineHighlight` for its chrome), with light and dark
defaults like every palette token. A style's `syntax` map overrides any of them for one view by the
same names without the prefix (`keyword`, `string`, ..., `plain` for untokenized text,
`lineNumber`, `lineHighlight`, and `gutter` for the numbers' background, which is the view's own
background unless set).

Lines are laid out in rows of eighty (a highlighted line is a row of its own), and only the rows in
view are realized, so a file of many thousands of lines lays out, shapes and draws just what is on
screen, and scrolling lays out nothing new until a row enters view. A view with a height of its own
(fixed, a weight or a fraction) scrolls inside it; a view sized to its content is as tall as all its
lines, so the page around it scrolls over the whole file, while only the rows that page shows are
realized. Rows not yet realized are estimated from the ones that have been measured, so the view's
height holds still as it scrolls. A selection is kept in the whole text, not in the realized rows:
select-all and copy take the entire file, and a selection survives the row it started in scrolling
away.

### Drawers and sheets

`Modal(side: ...)` opens its content from an edge instead of the centre: `LEFT` or `RIGHT` for a
drawer, `TOP` or `BOTTOM` for a sheet. The content spans that edge (full height for a drawer, full
width for a sheet) unless it sets that size, and slides in from the edge as it fades in, unless it
names its own `transition`, `enter` or `exit`.

```strata
Modal(open: menuOpen, side: "LEFT", onDismiss: action("state.set", name: "menuOpen", value: false)) {
  Panel(layout: { kind: "COLUMN", width: 280, padding: 16 }) { NavigationLinks() }
}
```

### Pointer cursor

`cursor` sets the pointer shape over a node and its descendants: `DEFAULT`, `POINTER`, `TEXT`,
`GRAB`, `GRABBING`, `MOVE`, `NOT_ALLOWED`, `RESIZE_EW`, `RESIZE_NS`, `RESIZE_NESW`, `RESIZE_NWSE`,
`CROSSHAIR`, `WAIT`, `PROGRESS`, `HELP` or `NONE`. Without one, editable and selectable text shows
`TEXT`, links `POINTER`, drag sources `GRAB` (`GRABBING` while dragging), movable boxes `MOVE`, and a
`SplitPane` divider the resize shape along its axis. Hosts read the shape with
`strata_surface_pointer_cursor`; the Win32 adapter and the desktop hosts apply it.

### Rotation

`rotate` turns a node clockwise by degrees about its `transformOrigin` (`{ x, y }` as fractions of
its bounds, the centre by default), after `scale` and before `translateX`/`translateY`. Like the
other presentation properties it moves nothing in layout, and `animateChanges`, `motions` and
keyframes can drive it. A rotated node's clip turns with it, it is hit within its turned shape,
and its blur or backdrop effect is masked to the turned shape too.

```strata
animation Spin { from { rotate: 0 } to { rotate: 360 } duration: 900ms; repeat: "forever"; }
Panel(style: style(width: 24, height: 24, radius: 12, border: { width: 3, color: theme.accent }),
      enter: Spin)
```

### Turning in depth

`rotateX` and `rotateY` turn a node in depth by degrees about its `transformOrigin`, seen from
`perspective` logical pixels away (800 by default; smaller is stronger), as CSS's `perspective()
rotateX() rotateY()` does: positive `rotateX` tips the top away, positive `rotateY` turns the right
side away. They compose with `scale`, `rotate` and the translations, animate like them, and the
node is hit within its projected shape, so the controls on a tilted card stay clickable. A node
turned in depth renders as a presentation group: the GPU applies its perspective to every vertex,
and text, images and shapes inside it interpolate perspective-correctly.

```strata
Panel(style: style(Card, rotateY: open ? 0 : -60, perspective: 600),
      animateChanges: { properties: ["rotateY"], policy: "bouncy" })
```

### Clip shapes and fading edges

A node that clips (`clip: true`, or a `Scroll`'s viewport) can cut its content to a shape other than
its rounded rectangle and fade it out toward its edges. `clipShape` is `circle` or `ellipse`
(filling the box), `diamond`, `hexagon` or `polygon` (a regular polygon, `clipSides` sides, a corner
up) or `star` (`clipSides` points); any other name keeps the rectangle. It can also be any shape a
`Draw` draws, placed in the node's box the same way (`{ kind: "sector", start: -90, sweep: 120 }`,
`{ kind: "polygon", points: [...] }`, a `path`). Circles, ellipses, rectangles, sectors and rings
are cut exactly per pixel; any other shape draws the node into a layer that its filled shape masks,
and a shape with only a stroke (an `arc` with a `strokeStyle`) cuts the node to that line. A shape
cuts the node itself as well, as `clip-path` does: its shadow, background and border take the shape
too, and so does what a pointer hits on it and within it (a named shape, or a shape with a fill),
though a node's own `hitShape` decides where that node itself is hit. `fadeEdges` takes `top`,
`right`, `bottom` and `left` distances in logical pixels over which content fades to nothing, so a
list dissolves into its edges as it scrolls instead of being cut. Both are evaluated with the clip,
under any transform.

```strata
Panel(style: style(width: 96, height: 96, clip: true, clipShape: "hexagon")) { Avatar() }
Panel(style: style(width: 96, height: 96, clipShape: { kind: "polygon",
      points: [{ x: 0.5, y: 0 }, { x: 1, y: 1 }, { x: 0, y: 1 }] })) { Photo() }
Scroll(style: style(FeedList, fadeEdges: { top: 40, bottom: 40 })) { Feed() }
```

### Text effects

Text styles take four effects. `textGradient` lists two or more colours drawn across the text from
left to right in place of its colour. `textOutline` (`{ width, color }`) rings each glyph,
`textGlow` (`{ radius, color }`) lights the space around it, and `textShadow` (`{ x, y, blur,
color }`) draws a softened copy under it. Outline, glow and shadow read the glyphs' distance field,
so text that uses them rasterizes as MSDF, with a field wide enough for the largest of them.

```strata
Text(text: "NEON", style: style(Title, color: #E8FBFFFF, textGlow: { radius: 9, color: #22C7FFCC }))
```

### World text and variable fonts

Text in any script draws without the app bundling a font for it. A character none of the text's
fonts has (an emoji, Arabic, Hebrew, Han, kana, Hangul, Devanagari) comes from the system's fonts:
the ones known for its script first (Segoe UI Emoji for emoji, Microsoft YaHei for Han), then any
installed font that covers it. Emoji draw in their own colours (COLR). Arabic letters take their
joined forms, and right-to-left runs are laid out in their order inside left-to-right lines, with
numbers and neutral punctuation placed as the Unicode bidirectional algorithm places them.

`font` can also name an installed font by its file, `"system:bahnschrift.ttf"`. On a variable font
`fontWeight` sets the `wght` axis instead of picking a face, and `fontVariation` sets any axis, as
`"wght 650, wdth 80"` (tags quoted or not); each value is clamped to the axis's range.

```strata
Text(text: "Headline", style: style(Title, font: "system:bahnschrift.ttf", fontWeight: weight,
                                    fontVariation: "wdth 80"))
```

### Animated images and video

An `Image` showing an animated GIF, APNG or WebP plays it: each frame shows for as long as the file
says (a GIF frame under 20 ms for 100 ms, as browsers do), and it loops as many times as the file
asks. It plays from its bytes, as browsers play one: a worker decodes a few frames ahead into one
texture while it shows, so neither creating the Surface nor the animation's length costs time or
memory up front. JPEG, PNG, GIF and WebP images all bind the same way, told apart by their bytes.

Video plays into a runtime image at its own size, so any `Image` (clipped, rounded, turned) can
show it.
`media.play` (`source`: a resource path, an absolute path or a URL; `image`: the runtime image id
to play into; `loop`, `muted`, `volume`) starts it with its sound, and `then` receives
`{ image, width, height, duration, position, paused, ended }` once the video is open.
`media.pause`, `media.resume`, `media.seek` (`seconds`), `media.set` (`muted`, `volume`, `loop`,
while it plays), `media.status` and `media.stop` take the same `image`. With no `source`, `media.play` plays what its event brings: the path a dialog chose,
the first file of a drop.

```strata
Button(label: "Play", onClick: then(
  action("media.play", source: "assets/app/intro.mp4", image: "video:intro", loop: true),
  action("state.setFromEvent", name: "intro")))
Image(image: "video:intro", layout: { width: 640, height: 360 }, fit: "CONTAIN")
```

Once a node has shown a video, the video pauses when no node names its image any more (its page
switched away, its dialog closed), as a browser pauses a video taken out of the page, and plays on
when one does again; scrolling it out of sight does not pause it, and a video the app paused stays
paused. Sounds from `audio.play` belong to the app rather than to a page, and play on until
stopped, like a music player's.

The Windows desktop host plays video through Media Foundation, in any format Windows can decode.

### Lottie

`Lottie` plays an After Effects animation exported as Lottie (Bodymovin) JSON. `source` is its
resource id; it is loaded through the host's resource loader the first time any widget names it and
parsed once, and each frame is evaluated from the parsed keyframes and drawn as vector paths, sharp
at any size and scale.

```strata
Lottie(source: "assets/app/success.json", loop: false, onComplete: action("state.set", name: "done", value: true))
Lottie(source: "assets/app/spinner.json", speed: 1.5, fit: "COVER", layout: { width: 48, height: 48 })
Lottie(source: "assets/app/toggle.json", segment: on ? [0, 30] : [30, 60], loop: false)
Lottie(source: "assets/app/reveal.json", playing: playing, progress: scrub,
       onProgress: action("state.setFromEvent", name: "scrub", field: "progress"))
Slider(min: 0, max: 1, step: 0.005, bind: scrub)
```

- `loop` (default `true`) plays the segment over and over; otherwise playback stops on its last
  frame and `onComplete` runs, its event carrying that frame.
- `autoplay` (default `true`) starts playback; `playing` (when given) plays or pauses it instead,
  and pausing keeps the playhead where it is and runs `onPause` with `{ frame, progress }` where it
  stopped. Playing again after the end starts over.
- `speed` (default `1`) scales the composition's frame rate; a negative speed plays backwards.
- `progress` (0-1) seeks: each new value moves the playhead there, as scrubbing a video does, and
  playback (if playing) goes on from it. A `Slider` bound to it scrubs; `onProgress`, which runs
  with `{ frame, progress }` each frame the playhead moves, setting it keeps the slider moving with
  the animation, as a video's seek bar does.
- Text layers in a font the file names by family wait for the system's font names, read once on a
  worker the first time any is needed, as a page's text waits for a web font, so the first frame
  with text never stalls.
- `segment: [first, last]` plays only those composition frames (`last` exclusive, as a layer's out
  point is); `first > last` plays them backwards. Changing the segment or `source` starts over.
- `fit` (`CONTAIN` by default, `COVER`, `FILL`, `NONE`, `SCALE_DOWN`) and `alignX`/`alignY` place
  the composition in the content box, as `Image` places an image; it is clipped to that box.

Playback runs on the widget's own clock, frame by frame, at the display's rate; reduced motion
holds the current frame.

Supported: the composition's size, rate and in/out points; shape, solid, null, image, text and
precomposition layers with parenting, in/out points, start time, stretch and time remapping;
transforms (anchor, position, also with separate x and y, scale, rotation, opacity, skew and skew
axis, auto-orient); hold, linear and bezier keyframes with per-dimension easing, spatial bezier
paths for positions (moving evenly along their arc) and keyframed paths. Shapes: groups with their
own transforms, rectangles (rounded), ellipses, stars and polygons (with roundness), paths; fills
and strokes (colour, opacity, fill rule, width, caps, joins, miter limit, dashes with offset);
linear and radial gradient fills and strokes with colour and opacity stops; trim paths (start, end,
offset, simultaneous or individual); repeaters (copies, offset, transform, start and end opacity,
above or below); round corners; merge paths (merge, add, subtract, intersect, exclude).

- Masks: add, subtract, intersect, lighten, darken and difference, inverted, with opacity,
  expansion and feather (keyframable, a Gaussian blur of the mask's edge that far each way).
- Track mattes: alpha, alpha inverted, luma and luma inverted, by the layer above or by `tp`. The
  matte layer is drawn as any layer is (its masks, opacity, effects, gradients, images and text
  included) and keeps the layer by its alpha or luminance pixel by pixel.
- Layer opacity fades the layer (or precomposition) as one picture: paints that overlap within it
  do not show through each other.
- Blend modes (`bm`): normal, multiply, screen, overlay, darken, lighten, color dodge, color burn,
  hard light, soft light, difference, exclusion, hue, saturation, color, luminosity and add, against
  what is under the layer within its composition. A precomposition is drawn apart, so its layers
  blend within it, and it is clipped to its box.
- Layer effects, in order: Fill, Tint, Tritone, Levels (individual controls), Drop Shadow
  (colour, opacity, direction, distance, softness, shadow only) and Gaussian Blur (blurriness,
  both, horizontal or vertical); other effects are skipped. Effects act on the masked layer before
  its matte, as in After Effects.
- Image layers (`ty: 2`): PNG, JPEG, GIF (first frame) or WebP assets embedded as data URIs
  (`e: 1`) or in files named relative to the animation (`u` + `p`), decoded once as the animation
  loads.
- Text layers (`ty: 5`): the text document (keyframed documents switch at their times) with font,
  size, fill and stroke colours, stroke width and stroke over fill, tracking, line height, baseline
  shift, caps and justification, as point text or paragraph text wrapped in its box. Fonts come
  from the composition's `fonts` list, matched to an installed font by PostScript name, else by
  family and style; letters it lacks fall back as other text does, and glyphs the composition
  exports (`chars`) are drawn in place of the font's. Text animators with range selectors (start,
  end, offset and amount, in percent or index; based on letters, letters excluding spaces, words or
  lines; square, ramp up, ramp down, triangle, round and smooth shapes with ease high and low)
  change position, anchor, scale, rotation, opacity, fill and stroke colour, stroke width and
  tracking.

Each paint is a vector path command, so a shape that only moves, turns or scales keeps its outline.
Masks resolve as geometry where that is exact: the masked content and its mask shapes are
intersected into regions of equal coverage. A layer that must be seen as one picture (to fade,
blend, run effects, take a matte or a feathered mask) is drawn apart as an isolated layer and
composited by the renderer on every backend, however deeply such layers nest; an animation with
blend modes or effects is itself drawn apart and cut to the widget's box. Precompositions may nest
64 deep and shape groups 64 deep; a document nested deeper is refused as it loads, so nothing in
one that loads is cut short. Expressions (they need a JavaScript engine) and 3D layers
and cameras are not played, as in other native players (Skottie, rlottie): expressions keep their
property's keyframed value and 3D layers draw flat.

### Talking to the operating system

A document reaches the operating system through actions. `system.openFile` (`title`, `filters`,
`multiple`, `folder`) and `system.saveFile` (`title`, `filters`, `name`) show the system's own
dialogs, modal to the window while the app behind them keeps running; filters are `{ name, patterns }` objects or `"Name|*.png;*.jpg"` strings. `system.notify`
(`title`, `body`) shows a notification from the app's tray icon, and `system.open` (`target`)
opens a file, folder or link in its default application. `window.minimize`,
`window.toggleMaximize` and `window.close` act on the window, and `window.setChrome` (`titleBar`:
`"custom"` or `"system"`; `backdrop`: `"mica"`, `"acrylic"`, `"tabbed"` or `"none"`; `dark`)
changes it: with a custom title bar the document draws its own, and with a backdrop the window
shows the system's material wherever the document draws nothing. `clipboard.copyImage` (`image`: a
resource path or runtime image id) puts an image on the clipboard; `clipboard.readImage` takes one
off it as a runtime image.

An answer arrives later, so `then(request, next)` runs `next` with it as its event once it does:
`{ cancelled, path, paths }` from a dialog, `{ image, width, height }` from `clipboard.readImage`.
`state.setFromEvent` keeps the whole answer, or one of its fields with `field`.

```strata
Button(label: "Open…", onClick: then(
  action("system.openFile", multiple: true, filters: [{ name: "Images", patterns: "*.png;*.jpg" }]),
  action("state.setFromEvent", name: "picked")))
```

Some things the host says unasked: a socket's message, a watched file's change, a tray menu's
choice. Each is said on a channel, and `strata.channel` (`options: { channel: name }`) hears it:
its action receives what was said as its event.

```strata
Panel(behaviors: [{ id: "strata.channel", options: { channel: "chat" },
                    action: action("state.setFromEvent", name: "lastMessage") }])
```

`match` narrows a listener to what has the given fields: `options: { channel: "tray", match: { id:
"quit" } }` hears only the tray menu's Quit. A node attaches a behavior once (the compiler
reports a second entry with the same `id`), so listeners for different matches sit on different
nodes (a wrapper component around the content works well).
`state.listAppend` with `fromEvent: true` appends what the event brings, or its `field`, so a
listener can keep a log: `action("state.listAppend", name: "log", fromEvent: true, field: "id")`.

Two behaviors take the rest. `strata.fileDrop` hears files dragged in from the shell: its action
receives `{ over, dropped, files }` as files arrive over the node, leave it, and are dropped on it.
`strata.windowDrag` makes a node a title bar: a press on it (not on a control inside it) moves the
window, with the system's snapping, and a double press maximizes or restores it.

```strata
Panel(style: TitleBar, behaviors: [{ id: "strata.windowDrag" }]) { Text(text: "My app") }
```

A host provides these through the `strata.platform` effect (see `strata.h`); the Windows desktop
host provides them all.

### The window, the tray and other windows

`window.set` changes the window; only the fields it is given change, and `then` receives the
window's state, `{ fullscreen, alwaysOnTop, x, y, width, height, maximized, minimized, visible,
opacity }` (with no fields it only reports it):

- `fullscreen`: borderless over the whole monitor the window is on; `false` puts it back where it
  was, maximized or not.
- `alwaysOnTop`, `title`, `resizable`.
- `width`, `height` (the client area) and `x`, `y` (the window's top-left on the screen), in
  device-independent pixels (1/96 inch, the logical units of a window that is not scaled to fit);
  `center: true` centres it on its monitor's work area. A fullscreen window leaves fullscreen to
  take a new size or place and goes back to it.
- `minWidth`, `minHeight`: the smallest client area the user can drag it to; a smaller window grows.
- `opacity` (0 to 1): the whole window, faded by the system's compositor.

A platform action that declares `fromEvent` takes that field from its event, so a slider can drive
the window directly: `Slider(onChange: action("window.set", fromEvent: "opacity"))`. `media.set`
takes it too (`fromEvent: "volume"`).

`window.hideToTray` hides the window and its taskbar button until its tray icon is clicked or
`window.show` runs; `window.show` also brings a minimized or covered window forward. `system.tray`
(`tooltip`, `icon`: a PNG, JPEG or other raster resource, `items`, `visible`) shows the app's tray
icon with a right-click menu of `items`, each `{ id, label, checked?, enabled?, separator? }`;
choosing one says `{ id }` on the `"tray"` channel to the session that set the menu. `visible:
false` removes the icon (a notification or a window hidden to the tray keeps it). The icon is the
one `system.notify` uses.

```strata
Button(label: "Tray", onClick: action("system.tray", tooltip: "My app", items: [
  { id: "show", label: "Show" }, { separator: true }, { id: "quit", label: "Quit" }]))
Panel(behaviors: [{ id: "strata.channel", options: { channel: "tray", match: { id: "quit" } },
                    action: action("window.close") }])
```

`window.open` (`id`, `root`, `title`, `width`, `height`, `x`, `y`) opens another window running
`root`, an `overlay` declared in the same module, as a Surface of its own: its own input, frames,
size and DPI, and its own dialogs, tray and window actions. It runs on the same host model as the
window that opened it (the showcase shares its `demo.*` data), and `then` receives `{ id, opened }`;
asking again for an open id brings that window forward. `window.close` with an `id` closes that
window (`then` receives `{ id, closed }`); with none it closes the window it runs in. Closing a
window the application began with ends it, and the windows its documents opened close with it.

```strata
overlay Inspector { root InspectorPanel() }
Button(label: "Inspector", onClick: action("window.open", id: "inspector", root: "Inspector",
                                            title: "Inspector", width: 420, height: 600))
```

`system.paths` answers where the user's folders are: `{ temp, home, documents, pictures, desktop,
downloads, appData }`.

### Following the system's look

`env.systemDark` is true while the operating system's app theme is dark, and `env.systemAccent` is
the user's accent colour (null where the host does not know it, so `env.systemAccent ?? #0078D4FF`
gives a fallback). Both change as soon as the user changes them, so a document that reads them
follows along without asking.

```strata
Panel(style: style(Card, background: env.systemDark ? #15181EFF : #F5F6F8FF,
                         border: { width: 2, color: env.systemAccent ?? #0078D4FF }))
```

A host reports them in the Surface environment (`system_appearance`, `system_accent` in
`strata.h`; hosts built before they existed read as a light theme with no accent).

### Dragging out and capturing

`strata.dragOut` (`options: { files, text, move }`) lets the user drag a node's files (`files`: a
list of paths) and text out of the window into other applications: a primary-button drag past the
drag slop starts the system's drag, which carries the files as a file drop (Explorer copies them,
a chat attaches them) and the text as text. Its action receives `{ effect }` once they are dropped:
`"copy"`, `"move"` (only with `move: true`, when the target moved the files), or `"none"` when the
drag was cancelled.

```strata
Panel(style: FileCard, behaviors: [{ id: "strata.dragOut", options: { files: [report.path] },
                                     action: action("state.setFromEvent", name: "dropped",
                                                    field: "effect") }]) { Text(text: report.name) }
```

`node.capture` (`key`, `path`, `clipboard`, `image`, `scale`) captures exactly what the keyed node
shows on screen: its rectangle as drawn (turned, scaled, cut by the scroll views around it, and
covered by whatever is above it) at the framebuffer's resolution, from the next presented frame.
`path` writes a PNG, `clipboard: true` copies it, `image` publishes it as a runtime image an
`Image` can show, and `scale` resizes it; asked for none of these, it is written to a new PNG in the
temporary folder. `then` receives `{ ok, width, height, path, image, copied, error }`.

```strata
Button(label: "Save a picture", onClick: then(
  action("node.capture", key: "chart", path: folders.pictures + "\\chart.png", image: "capture:chart"),
  action("state.setFromEvent", name: "captured")))
```

### Network, files and sound

`http.request` sends an HTTP or HTTPS request: `url`, `method` (`"GET"` unless given), `headers`
(an object), `body` (text as it is; an object or list goes as JSON, with its content type set
unless `headers` names one) and `timeout` (milliseconds, 30 s unless given). Its answer is
`{ ok, status, statusText, headers, body }`, `ok` meaning a 2xx status and `headers` keyed by
lower-case name; with `as: "json"`, or a JSON content type when `as` is left out, `json` holds the
parsed body instead of `body` (`as: "text"` keeps the text). A request that gets no response, or
whose JSON does not parse, answers `ok: false` with an `error`.

```strata
Button(label: "Load", onClick: then(
  action("http.request", url: "https://example.com/api/items", timeout: 5000),
  sequence(action("state.setFromEvent", name: "response"),
           action("state.setFromEvent", name: "items", field: "json"))))
Button(label: "Save", onClick: action("http.request", url: "https://example.com/api/items",
                                      method: "POST", body: { title: title, done: false }))
```

`socket.open` (`id`, `url`: `ws://` or `wss://`, `headers`, `protocols`) opens a WebSocket, and
everything it says arrives on the channel named by its `id`: `{ type: "open", protocol }` once
connected, `{ type: "message", text, json }` for each message (`json` only when the text parses as
JSON; a binary message brings `base64` instead), `{ type: "closed", code, reason }` when either
side closes it, and `{ type: "error", error }`. `socket.send` (`id`, and `text` or `json`; with
neither, the text its event brings) sends a message, and `socket.close` (`id`, `code`, `reason`)
closes it. `then` after `socket.open` receives `{ ok, id, protocol }` once it connects, or `error`.

```strata
Panel(behaviors: [{ id: "strata.channel", options: { channel: "chat" },
                    action: action("state.listAppend", name: "messages", fromEvent: true, limit: 100) }]) {
  Button(label: "Connect", onClick: action("socket.open", id: "chat", url: "wss://example.com/chat"))
  Button(label: "Send", onClick: action("socket.send", id: "chat", json: { text: draft }))
}
```

`file.read` (`path`, `as`: `"text"`, `"json"` or `"base64"`) answers `{ ok, path, size, modified }`
with `text`, `json` or `base64`; `modified` is in milliseconds since 1970, for `formatDate`.
`file.write` (`path`; `text`, `json` or `base64`; `append`; `createDirectories`) answers
`{ ok, path, size }`. `file.list` (`path`, `pattern` such as `"*.png"`, `recursive`) answers
`{ ok, path, entries }`, each entry `{ name, path, directory, size, modified }`, folders first.
`file.info` (`path`) answers `{ exists, directory, size, modified }`. `file.trash` (`path`) moves a
file or folder to the recycle bin, where it can be restored; it never deletes for good (the system
asks first when something is too large for the bin). Each answers `ok: false` with an `error` when
it fails. `file.watch` (`path`, `channel`, `recursive`) reports changes to a folder, or to one file,
on `channel` as `{ type, path }`, `type` being `"added"`, `"removed"`, `"changed"` or `"renamed"`
(which brings `oldPath` too); `file.unwatch` (`channel`) stops it.

A path is absolute, or a resource path such as `assets/app/notes.txt`, and `%VARIABLE%`s in it
expand (`%TEMP%`, `%APPDATA%`). With no `path`, each takes the one its event brings: the path a
dialog chose, the first file of a drop.

```strata
Button(label: "Open…", onClick: then(
  action("system.openFile", filters: ["Text|*.txt;*.md"]),
  then(action("file.read", as: "text"), action("state.setFromEvent", name: "opened"))))
```

`audio.play` (`source`, `id`, `volume`, `loop`, `muted`, `rate`) plays a sound or music file, or a
URL, in any format Windows can decode; `then` receives `{ ok, id, duration, position, paused,
ended }` once it is open. A sound without an `id` plays to its end and is gone, so a click can
sound many times over itself; with an `id`, playing again replaces the sound, and `audio.pause`,
`audio.resume`, `audio.set` (`volume`, `muted`, `loop`, `rate`, while it plays) and `audio.stop`
take the same `id`.

```strata
Button(label: "Tap", onClick: action("audio.play", source: "assets/app/tap.wav"))
Button(label: "Music", onClick: action("audio.play", source: "assets/app/theme.m4a", id: "music",
                                       loop: true, volume: 0.4))
```

All of it runs away from the frame: requests on worker threads, sockets and watches on threads of
their own. When a session closes, what it started stops with it, and nothing it asked is answered
afterwards. The Windows desktop host provides it through WinHTTP, the shell and Media Foundation.

### Following the pointer

The `strata.pointer` behavior tells a node where the pointer is over it. Its action receives an
object as the pointer moves over the node or any of its descendants: `inside`, `x` and `y` in
the node's own logical coordinates, `u` and `v` as fractions of its box, and its `width` and
`height`; once the pointer leaves, `{ inside: false, phase: "leave", pressed: false }`. Kept in state, it drives anything an
expression can: a card that tilts toward the pointer, a button that leans in.

Each report also says what the pointer did: `phase` is `press`, `release` or `move`, `pressed`
whether a press is held, `button` which (0 primary, 1 secondary, 2 middle), `clickCount` how many
quick presses in one place this is (2 on a double click), `shift`, `control`, `alt` and `meta`
which keys were down, and `surfaceX`/`surfaceY` where the pointer is on the surface, which a pan
or zoom under it does not change. A press begun over the node is reported until it is released wherever the
pointer goes, with `inside` false and `x`/`y` outside the box while it is out: enough for a marquee
selection, or for a wire drawn from a node.

```strata
component TiltCard() {
  state tilt = { inside: false, x: 0, y: 0, u: 0.5, v: 0.5, width: 0, height: 0 };
  Panel(
    style: style(Card, rotateX: tilt.inside ? (tilt.v - 0.5) * -24 : 0,
                       rotateY: tilt.inside ? (tilt.u - 0.5) * 24 : 0),
    animateChanges: { properties: ["rotateX", "rotateY"], policy: "snappy" },
    behaviors: [{ id: "strata.pointer", action: action("state.setFromEvent", name: "tilt") }]
  ) { Slot(name: "content") }
}
```

For light that follows the pointer, a material reads it directly on the GPU instead (see
`materialPointer` under [Authored materials](#authored-materials)): nothing in the interface
changes as the pointer moves.

### Scroll-linked effects

The `strata.scroll` behavior on a `Scroll` (or any node laid out as `SCROLL`, such as a `List`)
reports where it is scrolled after every layout that moved it, whatever moved it: the wheel, a drag,
an eased or programmatic scroll, a reveal, or content that grew. Its action receives `x` and `y`
(the offset), `progressX` and `progressY` (0 to 1 through the scroll range), `velocityX` and
`velocityY` (logical px/s, smoothed over consecutive frames), `contentWidth`, `contentHeight`,
`viewportWidth` and `viewportHeight`; when the scroll stops, it is told once more with zero
velocity. It reports in the same frame the scroll moved, so what it drives moves with the content:
a parallax layer, a reading-progress bar, a header that shrinks.

```strata
component Article() {
  state reading = { x: 0, y: 0, progressX: 0, progressY: 0, velocityX: 0, velocityY: 0,
                    contentWidth: 0, contentHeight: 0, viewportWidth: 0, viewportHeight: 0 };
  Panel(layout: { kind: "OVERLAY" }) {
    Scroll(behaviors: [{ id: "strata.scroll", action: action("state.setFromEvent", name: "reading") }]) {
      Panel(style: style(Hero, translateY: reading.y * 0.5))  // parallax: half speed
      Body()
    }
    Panel(style: style(Header, height: clamp(88 - reading.y * 0.5, 48, 88)))
    Panel(style: style(Bar, scaleX: reading.progressY, transformOrigin: { x: 0, y: 0.5 }))
  }
}
```

### Laying out by its own size

The `strata.size` behavior reports a node's laid-out logical size, `{width, height}`, after every
layout that changed it, so a component can choose its arrangement by the width it is actually
given (a container query) rather than the window's. The report lands in the frame that laid the node
out, and what it changes is laid out again in that frame. A layout that answers its own report by
flipping straight back to the size before is not reported again, so a narrow layout that makes the
node wide and a wide one that makes it narrow settle instead of oscillating.

```strata
component ProfileCard() {
  state size = { width: 0, height: 0 };
  Panel(style: Card, behaviors: [{ id: "strata.size", action: action("state.setFromEvent", name: "size") }]) {
    if size.width < 440 { StackedProfile() } else { SideBySideProfile() }
  }
}
```

### Flinging, snapping and bounds

`strata.movable` lets the user drag a node; its options shape what happens when they let go.
`fling: true` lets it coast on with the release velocity, slowing by `friction` (its speed falls by
e every 1 / friction seconds, 4 by default). `snapPoints` (a list of `{x, y}` offsets) or `snapX`
and `snapY` (offsets per axis) make it settle onto a snap point on a spring (the `spring` option
names the motion policy, "spring" by default): the release velocity carries it as far as friction
would before the nearest point to that is chosen, so a flick passes the nearest point. `bounds` is
`"parent"` (the default), `"none"` or limits on the offset (`minX`, `maxX`, `minY`, `maxY`, any of
them); past explicit limits the drag meets rubber-band resistance and springs back on release
(`rubberBand: false` stops it dead; with `"parent"` bounds it is off unless asked for). `axis: "x"`
or `"y"` keeps it on one axis.

Its action receives `moved` events: `phase` `move` while dragged, `end` on release, `settling` each
frame it coasts or springs, and `settle` once it comes to rest, with `snapped` (the index of the
snap point it took, or of the `snapX`/`snapY` entry, with `snappedX`/`snappedY` per axis), `x` and
`y` (the offset), `dx` and `dy` (how far it moved since the last report) and `bounds`. A press
catches a settling box where it shows.

With `controlled: true` the box is placed by its action: it shows no offset of its own, and the
action moves the state its layout reads by each report's `dx` and `dy`, so whatever else reads
that state (a wire drawn to the box, a guide) follows it. A cancelled drag reports the way back; a
controlled box does not coast.

```strata
Panel(layout: { position: "ABSOLUTE", inset: { left: node.x, top: node.y } },
      behaviors: [{ id: "strata.movable", options: { controlled: true }, action: e => {
        undoStep("Move node", node.id)
        node.x += e.dx
        node.y += e.dy
      } }])
```

```strata
// A bottom sheet laid out half open, with open, half and tucked-away snap points.
Panel(style: Sheet, behaviors: [{
  id: "strata.movable",
  action: action("state.setFromEvent", name: "sheet"),
  options: { axis: "y", fling: true, snapY: [-130, 0, 132], bounds: { minY: -130, maxY: 132 } }
}]) { SheetContent() }

// A card flung off to either side, or springing back to the middle.
Panel(style: Card, behaviors: [{
  id: "strata.movable",
  options: { fling: true, bounds: "none", snapPoints: [{ x: -520, y: 0 }, { x: 0, y: 0 }, { x: 520, y: 0 }] }
}])
```

### Anchored portals

A portal is absolutely positioned in the root application-surface plane, measured against the root
viewport, excluded from its parent's row/column/grid flow, and rendered as a detached root. It may
overlap unrelated content and is not constrained by an ancestor scroll viewport. `anchorTarget`
names a stable widget key (or `"parent"`/`"root"`). `anchorPoint: { x, y }` supplies an absolute
logical point instead and is used by pointer-positioned context menus.
Placement is backend-independent:

```strata
Panel(
  layout: {
    kind: "PORTAL",
    portalTarget: "root",
    detachFromParentClip: true,
    anchorTarget: "account.trigger",
    anchorSide: "BOTTOM",
    anchorAlign: "END",
    anchorGap: 6,
    anchorCrossOffset: 0,
    anchorFlip: true,
    anchorShift: true,
    matchAnchorWidth: true
  }
) {
  AccountPopup()
}
```

Sides are `TOP`, `BOTTOM`, `LEFT`, and `RIGHT`; alignment is `START`, `CENTER`, or `END`.
`anchorAlign` aligns along the cross axis and `anchorCrossOffset` applies a signed logical offset
after alignment. Flip changes the main-axis side when the preferred side cannot fit and the opposite
side has more room. Shift clamps the final result to the root viewport. Anchored portals participate
normally in retained rendering, hit testing, focus, semantics, clipping, motion, and nested portal
placement.

### Tone

`tone` is how a subtree's ink answers the brightness of what is behind it. It is a number from 0 to
1, set like any other style property, and inherited: a node that sets `tone` owns it for everything
it contains until a descendant sets its own.

```strata
style OnLightBackdrop { tone: 1; }
Panel(style: style(Card, tone: onLight ? 1 : 0), animateChanges: { properties: ["tone"] })
```

At 0 colours are drawn as authored. Author for a dark backdrop: light ink, at whatever opacities
give the hierarchy. As tone rises to 1, every light, unsaturated colour in the subtree (text, vector
shapes, fills, borders, overlays) has its lightness mirrored toward dark ink. Saturated colours keep
their hue and dark colours stay dark, so an accent, a status colour, or ink that was authored dark on
a bright control is left alone. Images and shadows are never toned. To exempt part of a toned
subtree, give it `tone: 0`.

Tone is presentation, like opacity: it rides the node's GPU presentation group, so animating it
re-plans nothing, and `animateChanges`, `motions` value channels and keyframes can all drive it. A
node that sets a tone renders as a presentation group for as long as it does, which is why it is
set on a surface rather than on every label. A node's BACKDROP effect is told its tone through
`effectTone()`, so glass can hold its body in the band its content's ink needs.

### Exterior shadows

`shadows` adds up to four ordered exterior shadow layers to any styled widget. Each layer is
rendered behind the widget and its backdrop effect, outside the source silhouette rather than
darkening translucent content:

```strata
style FloatingGlass {
  radius: 18;
  shadows: [
    { color: #0000002E, radius: 18, spread: 1, offsetY: 7 },
    { color: #00000018, radius: 5, spread: 0, offsetY: 2 }
  ];
}
```

`radius` is the blur extent; `spread` expands or contracts the shadow silhouette; `offsetX` and
`offsetY` move it in logical pixels. A widget's own clip does not cut off its shadow, while an
ancestor clip still constrains the composed subtree.

## Paints and gradients

`background`, `fill`, `track`, `scrim` and a border's `paint` take a paint: a colour, or a
linear, radial or conic gradient authored in the filled shape's normalized space, where `(0,0)` is
its top-left corner and `(1,1)` its bottom-right one. A gradient therefore survives resizing,
motion and fragment reuse without being re-authored.

```strata
style Card {
  background: {
    kind: "linear",
    angle: 155,               // 0 points up, angles advance clockwise, resolved against the
    stops: [                  // shape's aspect so the painted angle is the visual one
      { color: #1B2338FA, offset: 0 },
      { color: #141A2BFA, offset: 0.55 },
      { color: #191430FA, offset: 1 }
    ]
  };
}

style Glow {
  background: {
    kind: "radial",
    center: { x: 0.5, y: 0.2 },
    radiusX: 0.9, radiusY: 0.6,   // or one `radius` for both
    extend: "clamp",              // clamp | repeat | mirror
    stops: [ { color: #7CFFDA55 }, { color: #7CFFDA00 } ]
  };
}
```

A linear axis can also be given explicitly as `from`/`to` instead of `angle`. Stops are ordered,
between two and sixteen, and `offset` may be omitted to spread them evenly.

A `conic` gradient sweeps its stops once around `center` (the middle by default), clockwise,
starting from `angle` degrees (0 points up, as for a linear angle; 0 by default). The angle is
measured in the shape's own pixels, so a quarter of the sweep is a quarter turn on screen however
wide the shape is. Repeat a first stop's colour as the last to close the sweep without a seam, or
put two stops at one offset for a hard edge, as in a pie or a checker:

```strata
style Wheel {
  background: {
    kind: "conic",
    center: { x: 0.5, y: 0.5 },
    angle: 0,
    stops: [ { color: #FF0055FF }, { color: #FFD500FF }, { color: #00E5A0FF },
             { color: #2E7BFFFF }, { color: #FF0055FF } ]
  };
}
```

Gradients fill panels by geometry whose vertex colours reproduce the ramp exactly at every stop
(a hard stop stays a hard edge), so they cost no shader switch and behave identically on every
backend; a conic sweep is cut into wedges of at most two degrees, far finer than one colour level.
Drawn shapes evaluate their gradients per pixel instead (see [Vector drawing](#vector-drawing)).

A `border` takes a paint too: `border: { width, paint }` strokes the node's rounded outline with
any paint, laid over the node's own bounds like its background, and `paint` wins over `color`. The
border covers the fill beneath it, as a solid border does, so a translucent gradient ring does
not show the background through it.

```strata
style GlowCard {
  radius: 16;
  background: #141A2BFF;
  border: { width: 2, paint: { kind: "conic", stops: [
    { color: #7CFFDA00 }, { color: #7CFFDA00, offset: 0.6 }, { color: #7CFFDAFF } ] } };
}
```

`paintRotate` turns a node's gradient paints (its `background`, `border`, `fill`, `track` and
`scrim`) clockwise by degrees about the node's centre: a linear angle and a conic start advance by
it, and an explicit linear axis, a radial ellipse and an off-centre conic centre turn with it. It
is an animatable property, so keyframes, `motions` and `animateChanges` can drive it; a looping
animation spins a conic border without the interface changing:

```strata
animation BorderSpin {
  duration: 3000ms; easing: "linear"; repeat: "forever";
  from { paintRotate: 0 }
  to { paintRotate: 360 }
}
Panel(style: GlowCard, enter: BorderSpin) { CardBody() }
```

## Vector drawing

`Draw` paints authored outlines and images inside its own bounds, in the same normalized space
paints use. Every outline declares a `fill`, a `stroke`, or both; stroke widths and dash lengths are
logical pixels so they stay honest at any UI scale.

```strata
Draw(layout: { width: { weight: 1 }, height: 58 }, shapes: [
  { kind: "circle", center: { x: 0.5, y: 0.5 }, radius: 0.4, fill: #7CFFDAFF },
  { kind: "arc", center: { x: 0.5, y: 0.5 }, radius: 0.34, start: -90, sweep: 270,
    stroke: #56E1C6FF, strokeStyle: { width: 2, cap: "round" } },
  { kind: "polyline", points: hub.frameHistory,
    stroke: { kind: "linear", angle: 90, stops: [ { color: #56E1C6FF }, { color: #6E8BFFFF } ] },
    strokeStyle: { width: 2, cap: "round", join: "round" } },
  { kind: "path", commands: "M 0.22 0.66 C 0.36 0.30, 0.64 0.86, 0.78 0.42",
    stroke: #E8F1FFEE, strokeStyle: { width: 3, cap: "round", dash: [6, 4] } }
])
```

Shape kinds are `line`, `polyline`, `polygon`, `rect`, `circle`, `ellipse`, `arc`, `sector`,
`ring` and `path`. A `sector` is a slice of a ring between `innerRadius` and `radius` from `start`
sweeping `sweep` degrees (an `innerRadius` of 0, the default, is a pie slice); a `ring` is the
whole ring, `innerRadius` 0.3 unless given. A `path` outline uses the compact
`M`/`L`/`H`/`V`/`Q`/`C`/`Z` commands, in absolute or relative (lowercase) spelling, and is parsed at
compile time: a malformed outline is a compile error on the literal, not a shape missing at
runtime. `strokeStyle` carries `width`, `cap` (`butt`/`round`/`square`), `join`
(`miter`/`round`/`bevel`), `miterLimit`, `dash` and `dashOffset`. A `points` list may come straight
from host data.

Every outline is drawn with exact coverage: each pixel gets the true share of it the shape covers,
so edges land on the geometry at any scale or rotation. Circles and ellipses (filled or stroked),
sectors and rings, arcs stroked with butt or round caps, lines, and rects whose corners are
circular on screen, all in a solid color without dashes, are evaluated analytically by the pixel
stage, so they stay exact while they animate and a stroke thinner than a pixel draws
proportionally lighter rather than a pixel wide. Every other outline (polygons, polylines, paths,
dashed or gradient-painted shapes, and imported SVG icons) becomes a coverage mask the way text
glyphs are, at the shape's device size and sub-pixel position, and cached; the GPU rasterizes it
in compute shaders from the outline's edges, or for a stroke from its centreline, which it widens
itself (joins, caps and all), so a shape that changes every frame costs the CPU only its flattened
path. Fills
follow their fill rule, strokes are a union, so a translucent stroke stays one even alpha where it
crosses itself, and shapes larger than an atlas page are drawn in seamless tiles. Gradient paints
are evaluated per pixel over the mask: every stop, `extend` mode, radial distance and conic angle is
exact.
Shapes of one `Draw` that meet along an edge, straight or curved (pie slices without a gap, the
rings of a sunburst, tiles, the pieces of a shape cut apart), are drawn as one picture: each pixel
on the seam is split between them by area, so nothing behind them shows through as a hairline, and
two colours meeting keep exactly their own shares of it. It holds at any size and inside toned
groups, and for the paths of an SVG image and a chart's stacked areas too. Shapes that touch at a
single point only are drawn apart; the one pixel there is never seen.
Inside a presentation group held at another scale (a node that sets a `tone`, or one whose scale
stays put while other motion runs), masks rasterize at the group's density, so a zoomed shape is
as sharp as one drawn at that size. A `Draw` takes up to 16,384 shapes and a path up to 262,144
segments, so a live series of tens of thousands of points is one `polyline`.

`animateChanges: { properties: ["shapes"] }` animates a `Draw`'s shapes: when they change and keep
their structure (the same kinds, list lengths and fields, with only numbers and colours differing),
every coordinate, radius, angle, stroke width, dash and colour interpolates with the motion
policy, so a gauge sweeps, a sparkline morphs and a circle grows. A change of structure draws the
new shapes at once.

```strata
Draw(animateChanges: { properties: ["shapes"], policy: "standard" },
     layout: { width: 64, height: 64 }, shapes: [
  { kind: "arc", center: { x: 0.5, y: 0.5 }, radius: 0.4, start: -90, sweep: progress * 360,
    stroke: theme.accent, strokeStyle: { width: 6, cap: "round" } }
])
```

An `image` shape maps an image's `source` region (normalized, as for `Image.source`) onto the
parallelogram whose top-left, top-right and bottom-left corners are its three `points`, multiplied
by `tint`. It draws anything `Image` can, including [runtime images](svg.md#runtime-images), so a
turned or sheared picture needs no pre-rendered asset:

```strata
Draw(layout: { width: 96, height: 96 }, shapes: [
  { kind: "image", image: "app:skin/steve", source: { u: 0.125, v: 0.125, width: 0.125, height: 0.125 },
    points: [{ x: 0.2, y: 0.1 }, { x: 0.9, y: 0.2 }, { x: 0.2, y: 0.8 }], tint: #D8D8D8FF }
])
```

## Authored materials

An application declares its own materials in its `*.schemas.json`, alongside its widgets and
actions:

```json
"materials": {
  "definitions": [
    {
      "id": "demo:aurora",
      "blendMode": "straight_alpha",
      "fallback": "strata:rounded_rect",
      "parameters": [
        { "name": "intensity", "materialType": "FLOAT", "type": { "kind": "number" },
          "required": true, "nullable": false, "aliases": [], "default": null },
        { "name": "tint", "materialType": "COLOR", "type": { "kind": "color" },
          "required": true, "nullable": false, "aliases": [], "default": null }
      ],
      "shaders": { "hlsl": "assets/strata/shaders/materials/aurora.hlsl" }
    }
  ]
}
```

`Panel(material: material("demo:aurora", intensity: 0.85, tint: #7C6BFFFF))` then shades that
widget. Parameters are packed into the vertex draw data in declaration order, occupying floats 8
to 13: floats 0 to 7 carry the drawn shape's size, softness, border width and corner radii, and the
last two carry the draw mode and material opacity, leaving six floats for authored parameters.

An authored source writes one function and nothing else:

```hlsl
float4 material(PixelInput input) {
    return float4(input.color.rgb, input.color.a * materialCoverage(input) * materialOpacity(input));
}
```

A material shades **fills only**. Text, images, borders and shadows drawn inside its scope keep the
built-in shading, so applying a material to a panel never disturbs the labels inside it. The
surrounding prelude supplies `materialSize`, `materialOpacity`, `materialTime`, `materialRadii`,
`materialDistance`, `materialCoverage`, `materialFloat`, `materialFloat2` and `materialFloat4` —
`materialDistance` and `materialCoverage` give the shape's own rounded silhouette, so a material
never restates the geometry it was applied to, and `materialTime` returns seconds since the surface
began presenting, for materials that animate. `materialPointer(input)` returns where the pointer is
in the shape's own logical coordinates (`xy`, its top-left being 0, 0) and in `z` 1 while the
pointer is over the surface, else 0. It is mapped through the shape's own screen-space derivatives,
so it holds under scale, rotation and perspective, and it costs nothing on the CPU: a spotlight or
an edge glow that follows the pointer (the showcase's `demo:spotlight`) redraws with no change to
the interface, even reaching across neighbouring cards.

The declaration is backend-neutral: a host compiles the source key it implements and reports the
rest as approximations. The Win32 desktop and headless D3D11 hosts implement `hlsl`; a backend that
lacks the declared source draws the solid approximation and reports
`STRATA.RENDER2D.MATERIAL_APPROXIMATED` once per material.

## Authored render effects

Materials shade one fill. Effects filter a framebuffer input and may therefore implement glass,
refraction, color grading, or subtree post-processing. Effects are typed ordered programs in the
same application schema:

```json
"effects": {
  "definitions": [{
    "id": "demo:optic-glass",
    "input": "BACKDROP",
    "parameters": [
      { "name": "blurRadius", "effectType": "FLOAT", "type": { "kind": "number" },
        "required": true, "nullable": false, "aliases": [], "default": null },
      { "name": "tint", "effectType": "COLOR", "type": { "kind": "color" },
        "required": true, "nullable": false, "aliases": [], "default": null }
    ],
    "passes": [
      { "kind": "BLUR", "radiusParameter": "blurRadius", "downsample": 2 },
      { "kind": "SHADER",
        "shaders": { "hlsl": "assets/shaders/effects/optic-glass.hlsl" } }
    ]
  }]
}
```

`input` is `BACKDROP` (pixels already behind the widget) or `CONTENT` (an isolated rendering of the
widget subtree). Passes execute in declaration order and are `BLUR` or `SHADER`. Blur accepts
literal `radius`/`downsample` values or parameter references. Parameters occupy at most sixteen
floats in declaration order; `FLOAT`, `INT`, `FLOAT2`, `FLOAT4`, and `COLOR` use one, one, two, four,
and four slots respectively. Missing, unknown, duplicate, and wrongly typed arguments are compile
errors.

Apply the program through the ordinary widget/style effect field:

```strata
Panel(effect: effect("demo:optic-glass", blurRadius: 18, tint: #7DB8FF35))
```

`BACKDROP` effects sample pixels already rendered behind the widget by default. A nested glass
surface can instead sample the framebuffer as it existed immediately before this Surface started
rendering:

```strata
Panel(effect: effect(
  "demo:optic-glass",
  backdropSource: "SURFACE",
  blurRadius: 18,
  tint: #7DB8FF35
))
```

`backdropSource` is `"CURRENT"` by default. `"SURFACE"` is useful for independently authored
nested glass: its filtered output composites into the current widget position, but its samples are
not polluted by parent fills or earlier effects from the same Surface. In an overlay host, the
incoming Surface framebuffer is the game, window, or lower UI layer already present in the target.
Only `BACKDROP` effects accept this argument; `CONTENT` effects always filter their isolated
subtree.

Live effects are evaluated every frame unless they set `refreshRate`. With a rate, the renderer
continues composing every application frame while reusing the latest filtered sample between
refreshes; animation time is absolute, so a sampled shader does not run in slow motion. A rate only
saves work when it is below the display's refresh rate, since keeping the sample costs a copy.
`refreshRate` is a generic effect argument rather than a declared shader parameter:

```strata
Panel(effect: effect("demo:optic-glass", refreshRate: 120, blurRadius: 18, tint: #7DB8FF35))
Panel(effect: effect("demo:optic-glass", refreshRate: "UNBOUNDED", blurRadius: 18, tint: #7DB8FF35))
```

Bounds, scale, parameters, geometry epochs, and target changes invalidate the retained sample
immediately. Numeric rates must be positive; `"UNBOUNDED"` requests evaluation on every frame.

An HLSL pass defines `float4 effect(EffectInput input)`. The host prelude provides
`sampleEffectSource`, `sampleEffectBackdrop`, `effectFloat`, `effectFloat2`, `effectFloat4`,
`effectColor`, `effectTime`, `effectOpacity`, `effectTone`, `effectDistance`, and `effectMask`.
`effectTone()` is the [tone](#tone) of the widget the effect belongs to, including while it animates. `EffectInput`
carries framebuffer `uv`/`pixel`,
logical pixels, and `localUv` within the affected widget. Samples are straight-alpha values; the
host premultiplies pass output, applies the rounded widget mask and effect opacity once at final
composition, and clips work to the intersection of the effect bounds and inherited scissor.

`sampleEffectSource` and `sampleEffectBackdrop` are defined inside the effect's bounds extended by
64 logical pixels; a coordinate beyond that reads the nearest defined texel. A pass can therefore
refract, reflect, or average its immediate surroundings, while the D3D11 host captures only that
region per effect instead of the whole framebuffer, so an effect costs its own area rather than
the window's.

The D3D11 desktop and headless hosts execute the full pass program. The reference software backend
executes declared blur passes, ignores authored shader stages, and then applies the same rounded
mask, opacity, and backdrop/content composition. This approximation is intentionally deterministic
rather than a claim of shader fidelity. Packet v16 carries ordered backdrop/content batches,
current/surface backdrop selection, active rounded-clip geometry, effect refresh-rate policy, a
backdrop probe token, and a bounded sixteen-float parameter block; presentation groups carry tone
beside their transform and opacity.
A `CONTENT` effect is an isolated layer, and since packet v21 the engine can composite it as
more than a plain fade: after its declared passes it runs built-in filters (a blur with separate
x and y radii, a colour matrix, a luminance gradient map, per-channel levels, and a drop shadow)
and combines with what is under it by a composite operator: source-over, the fifteen W3C blend
modes, plus, destination-in and -out by alpha or luminance (a matte), and channel-wise maximum,
minimum and difference (coverage masks). Every backend, the reference renderer included, runs
these the same way; `Lottie` draws its blend modes, mattes, feathered masks and effects with them.
`CONTENT` effects nest as deep as a document draws them, and every level is drawn: a renderer
keeps the few innermost open layers in framebuffer-sized targets and parks the rest in targets
sized to what is read back from them, so memory follows the layers' own sizes, not their depth.
The public decoder rejects malformed clip/effect state, bounds `CONTENT` nesting at 1024 levels
(`STRATA_RENDER_PACKET_MAXIMUM_CONTENT_EFFECT_DEPTH`, a sanity bound on the packet's shape that no
compiled document reaches, not a drawing limit), and caps rounded clip stacks at sixteen.

## Local retained and derived state

State belongs to the keyed component instance that declares it.

```strata
state count = 0;
state enabled = true;
state tags: List<string> = ["account", "privacy"];
state record: Record = { title: "Ready", saved: false };
derived tagSummary = join(tags, " · ");

Button(label: format("Count {0}", count), onClick: action("state.adjust", name: "count", amount: 1))
Toggle(checked: enabled, onChange: action("state.setFromEvent", name: "enabled"))
Button(label: "Advanced", onClick: action("state.listToggle", name: "tags", value: "advanced"))
Button(label: "Saved", onClick: action("state.recordSet", name: "record", field: "saved", value: true))
```

`state.set`, `state.setFromEvent`, `state.adjust`, `state.toggle`, and `state.reset` are compiler-
known actions. Lists additionally support append, insert, remove by value/index, toggle membership,
and clear; records support checked field updates. `state.listAppend` appends its `value`, or with
`fromEvent: true` the event that runs it (one of its fields with `field`), and with `limit` keeps
only that many of the newest items: a log of what a socket or a watched folder says. List element types and a `Record` initializer's
fixed fields are validated at compile time: a `state.*` action must name a declared state, its value
must fit that state's type where the state is in scope, and reading or setting a field a record does
not declare is an error. A name that is not a state, parameter, local value or declared host root is
an error once the application schemas declare their host roots; without schemas, a near miss of a
name in scope is still reported. Derived declarations are pure, evaluated once per
rebuild, and rejected when their dependency graph cycles. Their exact action payload fields and
policies are generated from `DslActionRegistry`.

## Conditions and bounded loops

```strata
if env.compact {
  CompactHeader()
} else {
  WideHeader()
}

for item, index in projects.rows where item.visible {
  Button(key: item.key, label: format("{0}. {1}", index + 1, item.name))
}

when mode {
  "compact" -> { CompactHeader() }
  "wide" -> { WideHeader() }
  else -> { DefaultHeader() }
}
```

Conditions must be boolean. Host collections declare a maximum item count; unbounded or oversized
loops are rejected. A loop may bind its zero-based index and apply a boolean `where` filter. Dynamic
children should use stable host-provided keys. Multi-branch `when` requires exactly one final
`else`, so variant selection stays exhaustive.

Large or variable-height results use a keyed `Repeater` whose body contains exactly one `for`.
Unlike an eager loop, the repeater accepts a bounded collection with thousands of items and builds
only the visible range plus overscan. Every record must expose a stable `key` or `id`.

## Expressions and bindings

Expressions include property/index access, function calls, unary/binary operators, conditional
`?:`, null fallback `??`, lists, and objects. A property its value does not have reads as null, and
reports a diagnostic unless it is on the left of `??`: `answer.error ?? ""` asks whether there is
one. Bindings resolve in this order: local state,
component parameters, declarations, and registered host roots such as `env.*` or application data.

Common pure helpers cover formatting, case conversion, trimming, number clamp/round/precision,
list size/join, numeric/color operations, actions, effects, materials, and animations. Helper
capability and argument errors are source-ranged compiler diagnostics.

Text helpers include `split(value, separator)` (an empty separator splits into characters),
`replace(value, search, replacement)` (every occurrence), `indexOf(value, needle)` (-1 when absent;
also finds an item of a list), `padStart` and `padEnd(value, length, fill?)`, and
`slice(value, start, end?)`, which takes text or a list and counts negative bounds from the end.
List helpers `first`, `last`, `reverse`, `find(source, predicate)` and `sum(source, selector?)`
take a list or derived view; `first`, `last` and `find` give one item typed as the source's items,
or null. `range(start, end, step?)` lists the numbers from `start` up to but not including `end`,
at most 10000 of them. `sqrt` and `pow(base, exponent)` complete the numeric helpers.

`formatDate(value, pattern, utcOffsetMinutes?)` formats a timestamp in milliseconds since
1970-01-01 UTC, shifted by an optional offset (the host knows the user's zone; pass it in).
Patterns use `yyyy`/`yy`, `MMMM`/`MMM`/`MM`/`M`, `dd`/`d`, `EEEE`/`EEE`, `HH`/`H`, `hh`/`h`,
`mm`, `ss`, `SSS` and `a`; quoted text is literal: `formatDate(sent, "EEE d MMM 'at' HH:mm")`.
Names are English unless `monthNames` (twelve) and `weekdayNames` (seven, from Sunday) give others;
the short forms are their first three characters.

`formatNumber(value, decimals?, grouping?, compact?, currency?, percent?, signDisplay?,
groupSeparator?, decimalSeparator?)` writes a number; every argument after the value is optional
and is usually named. Rounding is half away from zero on the number as written (2.5 is 3, 1.005 is
1.01). With nothing else, it keeps two decimals and no grouping, as it always has.

| Call | Result |
|---|---|
| `formatNumber(1234.5)` | `1234.50` |
| `formatNumber(1234567, 0, true)` | `1,234,567` |
| `formatNumber(1234.5, currency: "USD")` | `$1,234.50` (a currency groups, and takes its own digits: `¥1,235` for `"JPY"`; a code without a symbol is written first, `CHF 1,234.50`; any other text is the symbol itself) |
| `formatNumber(3400000, compact: true)` | `3.4M` (K, M, B, T, two significant digits unless `decimals` says otherwise) |
| `formatNumber(0.256, percent: true)` | `26%` (`decimals: 1` gives `25.6%`) |
| `formatNumber(5, signDisplay: "always")` | `+5.00` (`"auto"`, `"always"`, `"exceptZero"`, `"negative"`, `"never"`) |
| `formatNumber(1234.5, currency: "EUR", groupSeparator: ".", decimalSeparator: ",")` | `€1.234,50` |

Strata has no locale object of its own: English is the default, and a host or application that
knows the user's conventions passes the separators in (from a theme token, application data or
state), as it passes `monthNames` to `formatDate`.

`formatBytes(value, decimals?, binary?, decimalSeparator?)` writes a byte count in the largest unit
that keeps it at least one, trailing zeros trimmed: `formatBytes(1500000)` is `1.5 MB`, `512 B`
stays whole, and `binary: true` counts in 1024s (`1.5 KiB`). `formatDuration(value, style?)` writes
a span of milliseconds: `"clock"` (the default) gives `1:05:09` (or `5:09` under an hour),
`"short"` the two largest units that are not zero (`1h 5m`, `5m 9s`, `350ms`) and `"long"` spells
them out (`1 hour 5 minutes`). `timeAgo(value, now, style?)` says how far a timestamp lies from
`now` (both in milliseconds since 1970): `just now` within ten seconds, then `42 seconds ago`,
`3 minutes ago` or `in 2 days` in the largest whole unit up to years; `style: "short"` writes
`3m ago` and `in 2d`. Expressions read no clock, so `now` comes from state that a timer refreshes
from the wall clock: the `clock.now` action answers `then()` at once with `{ now, utcOffsetMinutes }`
(UTC milliseconds since 1970, and the local zone's offset from UTC, which `formatDate` takes), and
the text follows the state:

```strata
state now = 0;
Panel(timers: [{ id: "clock", every: 1s, action: then(action("clock.now"),
                                                    action("state.setFromEvent", name: "now", field: "now")) }]) {
  Text(text: "Edited " + timeAgo(document.editedAt, now))
}
```

Collection helpers accept bounded pure lambdas such as `item -> item.enabled`. View helpers are
`filter`, `map`, `sortBy`, `distinctBy`, `groupBy`, `flatten`, `takeWhile`, `window`, and `page`;
terminal helpers are `count`, `any`, and `all`. A derived view exposes `items`, `total`, `matched`,
`rangeStart`, `rangeEnd`, `cacheHits`, and `rebuilds`.

Each lambda is type-checked against the helper's signature: its parameter is an item of the
helper's `source`, so `filter(rows, row -> row.titel)` reports the misspelled field, and the
predicates of `filter`, `takeWhile`, `count`, `any`, and `all` must produce a boolean. The selectors
of `sortBy`, `distinctBy`, and `groupBy` and the transform of `map` may produce any value; `map`'s
result items take the transform's type.

Catalog-declared controls accept a typed binding shorthand, for example
`TextBox(bind: query)` or `Toggle(bind: enabled)`. The compiler lowers it to the widget's canonical
controlled property and change callback and rejects conflicts when either is also written.

## Actions

```strata
Button(
  label: "Save",
  onClick: action("application.save", document: current.id, force: false)
)
```

The action ID must be a static registered string. Payload fields are named and checked against the
same typed contract dispatched by the host. See [Actions](actions.md) for the native host boundary.

UI-local actions compose without a host callback: `sequence(first, ...)`, `parallel(first, ...)`,
and `chooseAction(condition, whenTrue, whenFalse)`. `state.setFromEvent` adapts the canonical event
value into local state. `reveal.request` is resolved after layout, so a sequence can change a filter
and then reveal/focus a newly materialized stable key. Domain effects remain typed host actions.

`delay(after, action)` runs its action once `after` (a duration) has passed, from the node that
scheduled it; if that node has left the tree by then, the step is dropped with it. A status that
clears itself is a sequence with a delay:

```strata
Button(label: "Save", onClick: sequence(
  action("state.set", name: "saved", value: true),
  delay(2s, action("state.set", name: "saved", value: false))
))
```

`timers` runs actions on a clock, from the node that declares them, for as long as it is in the
tree. Each timer has a literal `id`, an `every` interval and an `action`; it fires once an interval
has passed and again each interval after, or only once with `repeat: false`. `running: false`
pauses it, and when it runs again (or its interval changes) the interval starts over. The action
is the node's current one, so `value: step + 1` counts on from wherever `step` is. A frame that
arrives late fires a timer once, never a burst of the intervals it missed, and a timer stops when
its node leaves the tree.

```strata
state step = 0;
state playing = true;
Panel(timers: [
  { id: "advance", every: 2s, running: playing, action: action("state.set", name: "step", value: step + 1) },
  { id: "hint", every: 10s, repeat: false, action: action("state.set", name: "hintShown", value: true) }
]) {
  Chart(kind: "BAR", series: [{ name: "Sales", values: datasets[step % 4] }])
}
```

`clipboard.copy(text:)` writes text to the host clipboard (or the surface's local clipboard when the
host installs none), where a paste reads it. `clock.now` reads the wall clock and answers `then()`
at once with `{ now, utcOffsetMinutes }`: a timer that runs `then(action("clock.now"),
action("state.setFromEvent", name: "now", field: "now"))` keeps `now` current for `timeAgo` and
`formatDate`.

## Animations and timelines

```strata
animation ContentMotion {
  duration: 180ms;
  from { opacity: 0; translateY: 8; }
  to { opacity: 1; translateY: 0; }
}

animation FocusMotion {
  duration: 90ms;
  from { scale: 1; }
  to { scale: 1.03; }
}

Panel(transition: ContentMotion, move: ContentMotion, stagger: 24ms)
Panel(motions: [
  { id: "expanded", target: expanded, animation: ContentMotion },
  { id: "focus", interaction: "FOCUS_VISIBLE", animation: FocusMotion },
  { id: "radius", property: "radius", target: expanded ? 12 : 4, policy: "fast" },
  { id: "surface", property: "background", target: expanded ? #345A8AFF : #1F2937FF, policy: "standard" }
])
Panel(animateChanges: { properties: ["background", "radius", "opacity", "translateY"], policy: "standard" })
Panel(animateContentSize: { width: false, height: true, clip: true, policy: "standard" })
Panel(disclosure: { expanded: expanded, collapsedExtent: 32, policy: "standard" })
Panel(contentKey: selectedId, contentTransition: ContentMotion) { SelectedDetails() }
Button(label: "Details", onClick: action("layer.push", name: "details", transition: ContentMotion))
```

Between `from` and `to`, an animation may set keyframes at any offset, written as a number
between 0 and 1 before a block. Each keyframe sets some of the properties `from` and `to` animate,
and every property interpolates through the keyframes that set it:

```strata
animation Heartbeat {
  duration: 1100ms; repeat: "forever";
  from { scale: 1 }
  0.14 { scale: 1.24 }
  0.28 { scale: 1 }
  0.42 { scale: 1.14 }
  to { scale: 1 }
}
```

`easing` takes a named curve (`linear`, `ease`, `ease-in`, `ease-out`, `cubic-in`, `cubic-out`,
`cubic-in-out`), any CSS curve as `"cubic-bezier(x1, y1, x2, y2)"` (x1 and x2 within 0..1, y free,
so it can overshoot), or a spring: `"spring(stiffness, damping)"` (unit mass) or one of the named
springs `spring` (gentle), `bouncy` (overshoots and rings) and `snappy` (quick, barely over). A
spring runs its own motion over its settling time, scaled to the animation's duration, and lands
exactly on its target. The default motion policy has the same three springs as policies, so
`animateChanges` and value `motions` can use them: `policy: "bouncy"`.

Animation timing properties, triggers, and animatable values are catalog-derived in the generated
reference. `transition` plays one timeline forward on insertion and backward on removal, reversing
continuously from its current position when visibility changes mid-flight. Use separate `enter` and
`exit` timelines only for deliberately asymmetric motion; mixing either with `transition` is a
compile error. Exit motion retains disappearing nodes until its finite timeline completes;
keyed `move` uses layout deltas. `stagger` delays an item's entry (`enter`, or the entry half of
`transition`) by its emitted position in the enclosing `for` loop: filtered-out items leave no gap
and positions past 16 enter together. The offset is latched when an entry starts, so later
reordering never restarts it; exits are not staggered. Layer push/pop/replace and modal transitions
use the named animation timing. Existing focus/state restoration is unchanged
across transitions. Reduced-motion policy is supplied through `env.reducedMotion` and snaps these
presentation transitions to their final state.

A host that stops presenting a retained Surface and later shows it again (a toggled overlay, a
game menu) calls `Surface::reveal()` (`strata_surface_reveal`). Entry motion, including authored
delay and stagger, replays from the next frame as if the tree had just been inserted. Nothing is
rebuilt: state, focus, scroll and layout are kept, and looping, interaction and target channels
continue undisturbed.

`motions` separates activation from reusable presentation timing. Each attachment has a stable
literal `id`, one named animation, and exactly one activation source: a Boolean `target` expression
or the `HOVER`, `PRESSED`, `FOCUS`, or `FOCUS_VISIBLE` framework interaction. `FOCUS` is
semantic focus and remains active after pointer acquisition; `FOCUS_VISIBLE` follows the
keyboard/spatial indicator policy and is normally the right source for visual focus treatment.
Boolean targets reverse continuously
from the currently displayed value when interrupted. Initial composition and reduced-motion mode
snap to the target endpoint without starting a clock. Independent channels may animate disjoint
properties; assigning one property to multiple channels, or embedding a legacy `trigger` in a
channel timeline, is a compile error rather than an order-dependent override.

A `motions` entry with `property` and a numeric or color `target` is a typed value channel; it does
not require duplicated endpoints. `animateChanges` is the opt-in implicit path for concrete
resolved style/layout values and retargets from the current presentation after rapid state, theme,
or compatible source-reactivation changes. Fixed, percentage, and fill-unit width/height targets interpolate without
being flattened to pixels; `placementX`/`placementY` animate the corresponding layer-placement
axis. Per-edge `marginLeft`/`marginTop`/`marginRight`/`marginBottom` and padding properties resolve
from the authored edge object. Unit changes and unsupported non-concrete endpoints snap rather
than passing through dimensionally invalid intermediate values.

`animateContentSize` follows measured child geometry on content/auto axes. `disclosure` adds the
expanded target, collapsed extent, clipping, and immediate descendant input/focus exclusion.
`contentKey` plus `contentTransition` retains the outgoing keyed child while the incoming child and
container size coordinate. Replacement defaults to `contentTransitionMode: "OUT_IN"`, which uses
the authored duration as one sequence and prevents outgoing/incoming text from sharing a baseline.
Use `"TOGETHER"` explicitly for image/surface crossfades where simultaneous presentation is wanted.
Fixed/fill/percentage axes remain owned by layout and cannot be silently
overridden by content-size motion. All named `policy` values resolve through the effective theme's
`UiMotionPolicy`; reduced motion reaches the same final style and layout in the same frame.

During compatible source reactivation, an already-running single-run keyframed timeline remains
compatible when it owns the same animation properties and keeps the same reverse model. Duration, delay, easing, fill mode,
keyframe offsets, and keyframe values may change compatibly: the runtime preserves the exact
currently displayed numeric/color values and blends them into the edited timeline. Changing the
owned property set, reverse mode, or repeating timeline is structural and restarts it from its new
initial frame. Target-value, resolved-style, and measured-size channels always retarget from their
current presentation value because their stable channel identity already defines compatibility.

### Shared-element transitions

A node with a `sharedKey` style that appears in the same rebuild in which a node with the same key
left (a thumbnail on one page and the large image on the next, a list row and its detail view)
flies from where the leaving node last showed, its position and size as presented, to its own box,
on the motion policy its `sharedTransition` style names ("standard" by default; "spring" springs).
It is a FLIP: the new node lays out where it belongs, starts transformed onto the old one's
rectangle (scroll offsets and ancestors' transforms accounted for), and eases its translation and
scale back to none. While it flies it paints above everything, outside the clips of its ancestors
(a scroll view does not cut it off), and lands back in its place among them. Going back flies the
other way the same way. Reduced motion skips the flight.

```strata
if selected < 0 {
  for photo in photos {
    Panel(key: photo.id, style: style(Thumb, sharedKey: photo.id, sharedTransition: "spring"),
          behaviors: [{ id: "strata.activate", action: action("state.set", name: "selected", value: photo.index) }])
  }
} else {
  Panel(style: style(Detail, sharedKey: photos[selected].id, sharedTransition: "spring"))
}
```

## Keys

Keys identify retained nodes across rebuilds. Use keys for dynamic/reordered children, focus
targets, forms, layers, and controlled state. Static leaves normally do not need keys. Keys are
required to be unique among siblings, which is the reconciliation boundary. The same model key may
appear under independent parents or component instances without manual string prefixes. A
surface-global lookup is intentionally unresolved when more than one active node has that key;
cross-tree focus, anchor, form, drag, and host automation targets therefore still need an
unambiguous application-level key. Duplicate explicit sibling keys remain invalid.

## Validation from the command line

Run the standalone production compiler without launching a window or game:

Validate all bundled applications through the configured CMake build:

```sh
cmake --build --preset linux-x64 --target strata_validate_modules
```

Use `windows-x64` on Windows. For one custom module, invoke the installed compiler directly:

```sh
strata_compile --check-module path/to/module.strata path/to/module.schemas.json
```

Prefix repeatable `--extension-path <directory>` options when the schema selects external native
packages. Use `--check-module-json` for the versioned `strata.diagnostics` document consumed by
editors. Built-in declarations come directly from the compiler's immutable native catalog; the
schema argument is optional and supplies only application declarations. Command-line diagnostics include the diagnostic code,
`file:line:column`, and message. Runtime diagnostic callbacks additionally carry the full source
range, component path, expected value, and occurrence metadata.
