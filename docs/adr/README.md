# Architecture Decision Records

Each significant decision gets a short record: the context, the decision, and its consequences.
Records are never edited once accepted. A later record **supersedes** an earlier one, and both
stay in the log.

| # | Decision | Status |
|---|---|---|
| [0001](0001-language-and-style.md) | C++20 embedded subset, Core Guidelines style | Accepted |
| [0002](0002-one-engine-many-targets.md) | One platform-free engine shared by tests, tools, Rack and firmware | Accepted |
| [0003](0003-dependency-pinning.md) | Pin every dependency and tool version | Accepted |
| [0004](0004-paged-tape.md) | Loop memory is a paged tape, pinned and never copied | Accepted |
| [0005](0005-bit-exact-floating-point.md) | Floating point is bit-exact across targets | Accepted |

## Template

```markdown
# NNNN — Title

- **Status:** Proposed | Accepted | Superseded by NNNN
- **Date:** YYYY-MM-DD

## Context
What forces are at play? What problem needs deciding?

## Decision
What we will do.

## Consequences
What becomes easier, what becomes harder, and what we must now watch for.
```
