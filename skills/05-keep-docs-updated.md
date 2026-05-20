# Skill: Keep Language Reference Up to Date

## Purpose

Whenever a new language feature is added or an existing feature is modified in PaykanLang,
the language reference documentation **must be updated in the same change**. This skill defines
exactly what to update and how.

---

## Files to Keep in Sync

| What changed | Files to update |
|---|---|
| New/changed syntax or semantics | `docs/language-reference.md` |
| New/changed ownership rules | `docs/language-reference.md` + `skills/02-ownership-system.md` |
| New/changed function/call rules | `docs/language-reference.md` + `skills/03-functions-and-calling.md` |
| New/changed types or builtins | `docs/language-reference.md` + `skills/01-language-basics.md` |
| New/changed compiler flags or CLI | `docs/language-reference.md` + `skills/04-compiler-and-implementation.md` |
| New/changed AST nodes, passes, or runtime | `skills/04-compiler-and-implementation.md` |

---

## Instructions

When implementing a feature or fix, follow these steps **before marking the task complete**:

1. **Identify what changed** — new syntax, new type, new rule, new builtin, new flag, etc.

2. **Update `docs/language-reference.md`**:
   - Add or update the relevant section(s).
   - Keep tables, grammar summaries, and examples consistent with the new behavior.
   - If a new construct is added, include a code example.
   - If a rule is removed or relaxed, remove or update the corresponding error entry in
     the *Semantic Checks* section.

3. **Update the matching skill file(s)** in `skills/`:
   - Mirror the same changes made to `docs/language-reference.md`.
   - Keep examples short and focused — skill files are read by AI, not humans.

4. **Update the Grammar Summary** in `docs/language-reference.md` if the grammar changed.

5. **Add or update sample programs** in `samples/` if the feature benefits from a concrete example.

---

## Checklist (run through this for every feature change)

- [ ] `docs/language-reference.md` updated
- [ ] Affected `skills/*.md` file(s) updated
- [ ] Grammar summary updated (if grammar changed)
- [ ] `samples/` updated (if a new sample helps)
- [ ] No stale error descriptions left in the *Semantic Checks* section
- [ ] All code examples in docs still compile and run correctly

---

## Example

> **Scenario:** A new `for` loop is added to the language.

- Add a `for` subsection under **Control Flow** in `docs/language-reference.md`.
- Add the `for` production to the **Grammar Summary**.
- Update `skills/01-language-basics.md` → Control Flow section.
- Add `samples/basic/for_loop.pkn` with a working example.
- Add any new semantic errors (e.g., `break`/`continue` inside `for`) to the Semantic Checks section.
