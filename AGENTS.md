# AGENTS.md

Guidance for AI coding agents (Cursor, Claude Code, Codex, Gemini, etc.) working in this repo.

- **Gemini CLI:** See [GEMINI.md](./GEMINI.md) for Gemini-specific instructions.

## Always implement on a feature branch, not on `main`

When executing a superpowers plan — or any multi-commit implementation task — the **first action** is to create and switch to a dedicated feature branch. Do not commit implementation work directly onto `main`.

```bash
git checkout -b feat/<plan-name>
```

Branch naming: lowercase-kebab, descriptive, ideally matching the plan file's stem. Examples from this repo's history: `feat/v2a-row-expand`, `feat/v2a-full-panel-render`.

Why: keeps `main` in a known-good state during the work, makes the feature reviewable as a single container merge at the end, and makes "discard and start over" cheap.

At the end of the plan, land the work with a `--no-ff` merge so the feature stays grouped:

```bash
git checkout main
git merge --no-ff feat/<plan-name>
git branch -d feat/<plan-name>
```

`main` commit [`5d6ef52`](.) is a worked example of the resulting shape.

### If you catch yourself already on `main`

Surface it to the user immediately and offer the rescue pattern:

1. `git branch feat/<name>` — label the current tip so commits aren't lost
2. `git reset --hard <commit-before-implementation-started>` — rewind `main`
3. `git merge --no-ff feat/<name>` — container merge
4. `git branch -d feat/<name>` — clean up the now-merged label

Only safe while nothing's been pushed to `origin`. If commits are already pushed, stop and ask before rewriting history.
