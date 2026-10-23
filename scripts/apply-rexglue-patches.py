#!/usr/bin/env python3
"""Apply the ordered ReXGlue SDK patch series to a Git checkout.

The patches are raw unified diffs, and may need Git's three-way merge support
when the target checkout has moved past the bundle's baseline. A complete
preflight is run in a temporary worktree before the real checkout is changed.
"""

from __future__ import annotations

import argparse
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path


PATCH_NAME = re.compile(r"^\d{4}-.+\.patch$")
BASELINE_COMMIT = re.compile(r"^SDK_COMMIT=(.+)$", re.MULTILINE)


def run(*args: str, cwd: Path, capture_output: bool = False) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        args,
        cwd=cwd,
        check=False,
        capture_output=capture_output,
        text=True,
    )


def require_success(result: subprocess.CompletedProcess[str], description: str) -> None:
    if result.returncode == 0:
        return
    output = "\n".join(part for part in (result.stdout, result.stderr) if part).strip()
    if output:
        print(output, file=sys.stderr)
    raise RuntimeError(description)


def git_root(repo_root: Path) -> Path:
    result = run("git", "rev-parse", "--show-toplevel", cwd=repo_root, capture_output=True)
    require_success(result, f"'{repo_root}' is not inside a Git worktree")
    return Path(result.stdout.strip()).resolve()


def has_tracked_changes(repo_root: Path) -> bool:
    return any(
        run(*command, cwd=repo_root).returncode != 0
        for command in (("git", "diff", "--quiet"), ("git", "diff", "--cached", "--quiet"))
    )


def patch_files(patch_dir: Path) -> list[Path]:
    patches = sorted(path.resolve() for path in patch_dir.iterdir() if path.is_file() and PATCH_NAME.match(path.name))
    if not patches:
        raise RuntimeError(f"no numbered .patch files found in '{patch_dir}'")
    return patches


def patch_state(repo_root: Path, patch: Path) -> str:
    if run("git", "apply", "--reverse", "--check", str(patch), cwd=repo_root,
           capture_output=True).returncode == 0:
        return "already applied"
    if run("git", "apply", "--3way", "--check", str(patch), cwd=repo_root,
           capture_output=True).returncode == 0:
        return "applicable"
    return "cannot apply"


def apply_series(repo_root: Path, patches: list[Path], *, apply: bool) -> tuple[int, int]:
    applied = 0
    skipped = 0
    for patch in patches:
        state = patch_state(repo_root, patch)
        if state == "already applied":
            print(f"skip    {patch.name} (already applied)")
            skipped += 1
            continue
        if state == "cannot apply":
            raise RuntimeError(
                f"cannot apply '{patch.name}' to '{repo_root}'; resolve or rebase this patch manually"
            )

        print(f"{'apply' if apply else 'check'}   {patch.name}")
        if apply:
            result = run("git", "apply", "--3way", "--index", str(patch), cwd=repo_root,
                         capture_output=True)
            require_success(result, f"failed to apply '{patch.name}'")
        else:
            # Mutate the disposable worktree so later patches are checked against
            # the result of earlier patches in the series.
            result = run("git", "apply", "--3way", "--index", str(patch), cwd=repo_root,
                         capture_output=True)
            require_success(result, f"preflight failed for '{patch.name}'")
        applied += 1
    return applied, skipped


def preflight(repo_root: Path, patches: list[Path]) -> tuple[int, int]:
    temporary_root = Path(tempfile.mkdtemp(prefix="rexglue-patches-"))
    worktree = temporary_root / "worktree"
    try:
        result = run("git", "worktree", "add", "--detach", str(worktree), "HEAD", cwd=repo_root, capture_output=True)
        require_success(result, "failed to create temporary preflight worktree")
        return apply_series(worktree, patches, apply=False)
    finally:
        if worktree.exists():
            run("git", "worktree", "remove", "--force", str(worktree), cwd=repo_root)
        shutil.rmtree(temporary_root, ignore_errors=True)


def print_baseline_warning(repo_root: Path, patch_dir: Path) -> None:
    baseline = patch_dir / "baseline.env"
    if not baseline.is_file():
        return
    match = BASELINE_COMMIT.search(baseline.read_text(encoding="utf-8"))
    if not match:
        return
    commit = match.group(1).strip()
    if run("git", "cat-file", "-e", f"{commit}^{{commit}}", cwd=repo_root).returncode != 0:
        print(
            f"warning: patch baseline {commit} is not available locally; "
            "using three-way application against HEAD instead.",
            file=sys.stderr,
        )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo-root", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--patch-dir", type=Path, help="directory containing the numbered patch files")
    parser.add_argument("--check", action="store_true", help="preflight only; do not modify the checkout")
    args = parser.parse_args()

    repo_root = git_root(args.repo_root.resolve())
    patch_dir = (args.patch_dir.resolve() if args.patch_dir else repo_root / "patches")
    if not patch_dir.is_dir():
        print(f"error: patch directory does not exist: '{patch_dir}'", file=sys.stderr)
        return 1

    try:
        patches = patch_files(patch_dir)
        print_baseline_warning(repo_root, patch_dir)
        print(f"==> Preflighting {len(patches)} patch(es) in a temporary worktree...")
        preflight_applied, preflight_skipped = preflight(repo_root, patches)

        if args.check:
            print(f"==> Preflight passed: {preflight_applied} applicable, {preflight_skipped} already applied.")
            return 0

        if has_tracked_changes(repo_root):
            print("error: tracked changes are present; commit, stash, or discard them before applying patches.", file=sys.stderr)
            return 1

        if preflight_applied == 0:
            print("==> All patches are already applied; no changes made.")
            return 0

        print("==> Applying patches to the current checkout...")
        applied, skipped = apply_series(repo_root, patches, apply=True)
        # Three-way application requires an index. The checkout was required to
        # have a clean index, so resetting it leaves only the intended working
        # tree changes for review and commit.
        require_success(run("git", "reset", cwd=repo_root, capture_output=True), "failed to unstage applied patches")
        print(f"==> Done: applied {applied} patch(es), skipped {skipped} already-applied patch(es).")
        return 0
    except RuntimeError as error:
        print(f"error: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())