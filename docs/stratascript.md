# StrataScript

StrataScript is the logic language of `.strata`. Expressions describe what the interface shows;
StrataScript says what happens: what a click, a drag or an answer from the platform does to the
application's state. It is part of the same language, compiled by the same compiler, checked by the
same type checker and shipped in the same artifact, so a document needs no host code to behave.

```strata
type Node = { id: string, title: string, x: number, y: number }

component Graph() {
  state nodes: List<Node> = []
  state selected: string? = null

  fn nodeAt(id: string) = find(nodes, n => n.id == id)

  fn addNode(title: string, x: number, y: number) {
    let id = `n${length(nodes) + 1}`
    nodes = [...nodes, { id: id, title: title, x: x, y: y }]
    selected = id
  }

  fn moveNode(id: string, dx: number, dy: number) {
    undoStep("Move node", id)
    nodes = map(nodes, n => n.id == id ? { ...n, x: n.x + dx, y: n.y + dy } : n)
  }

  root ZoomView(key: "graph") {
    for node in nodes {
      Panel(key: node.id, layout: { position: "ABSOLUTE", inset: { left: node.x, top: node.y } },
            behaviors: [{ id: "strata.movable", options: { controlled: true },
                          action: e => moveNode(node.id, e.dx, e.dy) }])
    }
  }
}
```

`samples/scripting.strata` exercises every part of the language and is run by the
`strata.headless.scripting` test.

## Design rules

- **One language.** A StrataScript expression is a `.strata` expression: the same operators,
  helpers, values and types. Scripts add functions, statements and effects around them.
- **Effects are explicit.** A function that writes state, performs or awaits an action, or calls a
  function that does, has effects. Such a function may be called only by a handler or by another
  function; calling it where a value is computed (a property, a `derived` value, a state's
  initializer) is a compile error, `STRATA.DSL.SCRIPT_EFFECT_IN_PURE_CONTEXT`.
- **A handler is a transaction.** What a handler writes is seen by its own later reads at once and
  committed together when it finishes: the interface rebuilds once. A handler that fails (a runtime
  error, a `throw` nothing catches, an exhausted budget) leaves state as it found it.
- **Deterministic and bounded.** A script reads the clock only through an action (`clock.now`), so
  a headless replay behaves the same. Every run has a step budget and a call-depth limit; a runaway
  loop is stopped and reported, never freezes the interface.
- **Typed, gradually.** Every value has a type the compiler knows or infers. A parameter without an
  annotation, an awaited answer or a caught error is `any`: it is checked where it is used.

## Values and types

Scripts use the `.strata` value kinds (`null`, `boolean`, `number`, `duration`, `string`, `color`,
`image`, `key`, lists and records) and add **functions** as values.

Types are written as in component parameters, and more:

| Type | Meaning |
|---|---|
| `number`, `string`, `boolean`, `any`, ... | the built-in types |
| `List<T>`, `Map<T>` | a list, a record of `T` values |
| `T?` | `T` or `null` |
| `"exec"` | exactly that string |
| `A \| B` | either; a union of strings is an enumeration (`"exec" \| "data"`) |
| `{ id: string, note?: string }` | a record; `note` may be left out |
| `(Pin, number) => boolean` | a function; parameters may be named for the reader, `(pin: Pin) => boolean` |
| `Name` | a type declared with `type` |

```strata
type Kind = "exec" | "data"
type Pin = { id: string, kind: Kind, label: string, note?: string }
type Check = (Pin) => boolean
type Tree = { label: string, children: List<Tree> }
```

`type` may be declared at the top of a file or in a component, screen or overlay body; every
declared type is visible throughout the program. A type that recurs (`Tree`) reads as `any` where it
recurs. Values are immutable: updating a record or list makes a new one, and assignment through a
path (`node.x = 4`) is shorthand for building that new value and assigning it.

## Declarations

```strata
fn area(w: number, h: number): number { return w * h }   // block body
fn twice(x: number) = x * 2                               // expression body
fn greet(name: string, greeting: string = "Hello") = `${greeting}, ${name}!`
async fn pick(title: string) { let path = await action("system.openFile", title: title); return path }
```

`fn` may be declared at the top of a file (visible to every module of the program) or inside a
component, screen or overlay, where it sees the body's parameters, states and derived values,
whichever comes first: a component's functions read and write the states declared after them.
Functions declared in one block see each other, so they may call one another and recurse.

A call binds arguments by position, then by name (`greet(greeting: "Hi", name: "Bo")`); a parameter
left out takes its default. A function's return type is inferred from an expression body and may be
stated after the parameters.

## Lambdas

```strata
n => n * 2
(a, b) => a + b
(pin: Pin) => pin.kind == "exec"
() => { count += 1 }
e => { if e.phase == "end" { commit(e) } }
```

A lambda captures what is visible where it is written: locals, parameters, states (by reference, so
it always reads the current value) and functions. A function can return a lambda that keeps its own
`var`s:

```strata
fn makeCounter() {
  var n = 0
  return () => { n += 1; return n }
}
```

The older single-parameter form `n -> expr` remains valid in helper arguments.

## Statements

```strata
let total = 0;              // immutable local
var count = 0;              // mutable local
count += 1;                 // = += -= *= /= %=  (+= also appends to a list)
node.x = 10;                // path assignment: rebuilds node with x = 10
nodes[2].title = "Branch";  // path assignment through a list index

if count > 3 { ... } else if count > 1 { ... } else { ... }
while count < 10 { count += 1 }
for item in items { ... }
for item, index in items where index > 0 { ... }
when kind {
  "exec" -> { ... }
  "data" -> { ... }
  else -> { ... }
}
break; continue; return value;
try { ... } catch error { ... }
throw "message";            // or any value
fn local(x: number) = x + 1 // a function in a block
```

Semicolons are optional at the end of a line. An expression statement whose value is an action
performs it (see Actions).

## Template strings

```strata
`count is ${count}, twice ${twice(count)}`
`a \` backtick and a literal \${not interpolated}`
```

A template string joins its text and the text of each `${expression}` in order; lists and records
read as they display.

## State

Inside a component, screen or overlay, its `state` names are assignable by handlers and by the
functions they call:

```strata
state count = 0
fn bump() { count += 1 }
Button(label: `${count}`, onClick: () => bump())
```

A handler reads its own writes at once; a state it has not written reads what the store holds now,
not what the interface last showed. Writes are checked against the state's declared type when they
are committed; a write the type rejects fails the handler and commits nothing.

## Handlers

Wherever an action is expected (`onClick`, `onChange`, a behavior's `action`, a timer's `action`, a
component parameter of type `action`), a function may be given instead. It is called with the value
the action's event carries:

```strata
Slider(value: volume, onChange: v => { volume = v; if v == 0 { muted = true } })
Panel(behaviors: [{ id: "strata.movable", options: { controlled: true },
                    action: e => { x += e.dx; y += e.dy } }])
```

A handler's parameter may be omitted (`() => ...`). A component parameter of type `action` can be
called like a function: `onPick()` runs the handler (or performs the action) it was given.

A `strata.movable` with `controlled: true` is placed by its handler: it shows no offset of its
own, and each report's `dx`/`dy` is how far to move the state its layout reads, so everything that
reads that state follows the drag.

## Actions

Actions stay the way the interface talks to the host and the platform. An expression statement of
action type performs the action, once the handler's writes are committed, and `await` performs it
and waits for its answer:

```strata
action("document.save", id: doc.id);                       // performed, not awaited
let answer = await action("clock.now");                    // answered at once
let picked = await action("system.openFile", title: "Open");      // answered later
```

`await` must be a statement's whole value (`let x = await ...`, `x = await ...`, `await ...;`,
`return await ...`), in an `async fn` or a handler. An async function must be awaited. Before the
action runs, what the handler wrote so far is committed. While a handler waits, the interface keeps
running; the handler resumes when the answer arrives. If the node that owns the handler leaves the
tree first, the handler is dropped.

## Undo

`undoStep("Move node")` makes what the handler commits one undo step with that label;
`undoStep("Move node", "drag")` merges it with the previous step of the same key, as a drag's steps
merge. A handler that names no step makes the history forget what came before, as an unlabeled
state change does.

## Errors

A runtime error (reading a missing field without `??`, a type mismatch, indexing outside a list) or
an uncaught `throw` ends the handler, discards what it wrote since its last commit, and reports a
diagnostic with the error's code and message. `try`/`catch` catches both; the caught value of a
runtime error is a record `{ code, message }`. A function that fails while computing a value outside
a handler reports `STRATA.SCRIPT.UNCAUGHT` and its value is `null`.

## Budgets

| Limit | Default |
|---|---|
| Steps per handler run (between awaits) | 10,000,000 |
| Call depth | 512 |

A handler that exhausts its step budget is stopped with `STRATA.SCRIPT.BUDGET_EXHAUSTED`.

## Library

Every expression helper (`map`, `filter`, `find`, `sum`, `sortBy`, `format`, `split`, ...) is
available to scripts, and accepts script lambdas. Scripts also have:

| Area | Helpers |
|---|---|
| Lists | `reduce(source, (acc, item, index) => ..., initial)`, `findIndex`, `insertAt`, `removeAt`, `replaceAt`, `unique`, `sort(source, (a, b) => number?)`, `zip`, `chunk` |
| Records | `keys`, `values`, `entries`, `fromEntries`, `has`, `without` |
| Numbers | `sin`, `cos`, `tan`, `atan2`, `hypot`, `sign`, `trunc`, `log`, `exp`, `lerp` |
| Text and values | `repeat`, `parseNumber` (null when the text is not a number), `toText`, `typeOf`, `toJson`, `parseJson` |
| Handlers | `undoStep(label, coalesce?)` |

A function a script declares shadows a helper of the same name.

## Diagnostics

| Code | Meaning |
|---|---|
| `STRATA.DSL.SCRIPT_ASSIGNMENT_TARGET` | the target is not a state or a `var` (or a path from one) |
| `STRATA.DSL.SCRIPT_JUMP_OUTSIDE_LOOP` | `break` or `continue` outside a loop |
| `STRATA.DSL.SCRIPT_RETURN_OUTSIDE_FUNCTION` | `return` outside a function or lambda |
| `STRATA.DSL.SCRIPT_AWAIT_CONTEXT` | `await` outside an async function or a handler |
| `STRATA.DSL.SCRIPT_AWAIT_POSITION` | `await` that is not a statement's whole value |
| `STRATA.DSL.SCRIPT_ASYNC_NOT_AWAITED` | an async function called without `await` |
| `STRATA.DSL.SCRIPT_EFFECT_IN_PURE_CONTEXT` | a function with effects called where a value is computed |
| `STRATA.DSL.SCRIPT_MISSING_ARGUMENT`, `..._UNKNOWN_ARGUMENT`, `..._DUPLICATE_ARGUMENT` | a call that does not fit the function |
| `STRATA.DSL.SCRIPT_NOT_CALLABLE` | a call of something that is not a function |
| `STRATA.SCRIPT.*` | runtime: `BUDGET_EXHAUSTED`, `CALL_DEPTH`, `COMMIT_REJECTED`, `ACTION_FAILED`, `EFFECT_OUTSIDE_HANDLER`, `UNCAUGHT`, ... |
