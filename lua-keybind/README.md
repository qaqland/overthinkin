# lua-keybind

## Context (ctx) lifecycle

A context is a temporary overlay of keybinds active while one or more
primary modifiers are held.

### Rule

- ctx is created when the first primary-modifier bind fires.
- ctx accumulates primary modifiers from each subsequent bind fire.
- ctx is destroyed when **all** accumulated primary modifiers are
  released.

Primary modifiers: CTRL, ALT, SUPER. SHIFT is secondary and does
not affect ctx lifetime.

### Simple example

```
bind(SUPER, "Tab", function(ctx)
    ctx:bind("~", function() msg("reverse") end)
end)
```

SUPER held → ctx created → ~ bound in ctx.
Release SUPER → ctx destroyed → ~ restored.

### Complex example

```
bind(ALT, "Tab", function(ctx)
    ctx:bind("~", function() msg("alt-then-~") end)
end)
bind(SUPER, "Tab", function(ctx)
    ctx:bind("~", function() msg("super-then-~") end)
end)
```

| Step | ctx-mod | ctx exists? |
|------|---------|-------------|
| ALT+Tab fires | {ALT} | created |
| SUPER+Tab fires | {ALT, SUPER} | alive |
| release ALT | {ALT, SUPER} | alive |
| release SUPER | {ALT, SUPER} | destroyed |

