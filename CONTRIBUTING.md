# Contributing

Start with a reproducible issue. State what you expected, what happened, the OS,
the commit tested, and the smallest scene or request that reproduces the problem.
Use fictional scene content; remove credentials, personal paths and conversations.

For agent or API changes:

```sh
cd ASSISTANT
npm ci --ignore-scripts
npm test
npm run build:runtime
```

For native changes, also build the editor and run CTest as described in
[the build guide](docs/BUILD.md). Renderer changes need a real decoded image and
the executed backend, not just successful compilation or a selected UI setting.

Keep pull requests focused. Explain the concrete behavior changed and include the
commands run and their results. Add a regression test for a nontrivial bug fix.
Never replace a failed check with a weaker assertion merely to make CI pass.

Do not commit user scenes, provider credentials, chats, crash dumps, machine paths
or agent operating instructions. Required product prompts and deterministic test
fixtures belong in source; private development prompts do not.

Commits and pull requests document work actually performed. Do not backdate them
or invent a development history. Preserve existing copyright and license notices.
Check [LICENSING.md](LICENSING.md) before contributing or redistributing code.
