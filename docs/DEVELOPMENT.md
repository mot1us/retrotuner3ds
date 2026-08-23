# Development guide

## Open in VS Code

Open `RetroTuner3DS.code-workspace`. The workspace starts terminals in the project
root and supplies the devkitPro environment used by this Mac.

Useful VS Code commands:

- **Terminal → New Terminal** opens a shell ready for Git and devkitARM.
- **Terminal → Run Build Task** builds `retrotuner3ds.3dsx`.
- **Terminal → Run Task → RetroTuner3DS: Host tests** runs the sanitizer suite.
- **Source Control** shows the same staged/unstaged changes as `git status`.

The recommended C/C++ extension is optional; it provides navigation and
diagnostics but does not replace the devkitARM compiler.

## Daily Git flow

Start new work from the latest tested `main`:

```sh
git switch main
git pull --ff-only
git switch -c fix/short-description
```

While working:

```sh
git status
git diff
./tests/run_live_host_tests.sh
make 3dsx -j4
```

Commit a focused result:

```sh
git add path/to/changed/files
git commit -m "fix: describe the behavior"
git push -u origin HEAD
```

Open a pull request into `main`. For playback work, record desktop test results
and the real-hardware result separately. A pull request can remain open while a
`.3dsx` candidate is being tested.

## Working with Codex

A productive request usually names the outcome and supplies the latest hardware
observation, for example:

> Fix B so one press returns from playback during buffering. Run host tests and
> build a hardware-test package, but do not merge until I test it.

Codex can inspect and edit the same working tree, run tests, prepare focused
commits, and summarize the exact hardware checks needed. You can review every
change in VS Code's Source Control view before it is committed or pushed.

Useful checkpoints to request are:

- “Show me the diff before committing.”
- “Commit this fix but do not push it.”
- “Push this branch and prepare a pull request.”
- “Tag the hardware-tested build and prepare a release.”

## Remotes

- `origin` is the public `mot1us/retrotuner3ds` repository.
- `upstream` is the original Video player for 3DS repository and is configured
  fetch-only to avoid accidental pushes.

To inspect upstream changes without merging them:

```sh
git fetch upstream
git log --oneline --left-right main...upstream/main
```
