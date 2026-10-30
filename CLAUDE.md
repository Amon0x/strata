# Working on Strata

- Nothing in this codebase is sacred. Existing designs may be badly made, badly thought out or half finished; don't preserve them out of respect for what is there. When something is wrong at the root, redesign or replace it rather than working around it.
- Do implementation yourself in the main session. Subagents are only for read-only exploration, never for writing or changing code.
- Build primitives, not features, and judge each gap on its own merits. A request is a probe of whether the core is expressive enough; the answer can go either way:
  - If it composes cleanly from what exists, write it in .strata. Composing it should feel reasonable, though: if it takes contortions, is slow, fragile, or needs per-frame description rebuilds, that is evidence a primitive is missing, not a reason to keep tryharding.
  - If a capability is genuinely missing, add it at the level that unlocks the most: the general mechanism (animate any value, lay out by a function, hit by a shape) over the one effect that exposed it. Weigh how much it unlocks against what it costs to carry in the core.
  - Convenience built on primitives is welcome when the pattern is common across many apps. Don't add things only one idea needs, or knobs whose only purpose is matching one reference.
