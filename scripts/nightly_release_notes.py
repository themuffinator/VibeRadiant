#!/usr/bin/env python3
from __future__ import annotations

import argparse
import datetime as dt
import subprocess
import sys
from pathlib import Path


def git_lines(args: list[str], repo: Path) -> list[str]:
    try:
        output = subprocess.check_output(["git", *args], cwd=repo, text=True, stderr=subprocess.STDOUT)
    except subprocess.CalledProcessError as exc:
        msg = exc.output if exc.output else str(exc)
        raise RuntimeError(msg) from exc
    return [line.rstrip() for line in output.splitlines() if line.strip()]


def commit_lines(repo: Path, commit_range: str | None) -> list[str]:
    if commit_range and commit_range != "HEAD":
        args = ["log", commit_range, "--pretty=format:- %h %s"]
    else:
        args = ["log", "-n", "25", "--pretty=format:- %h %s"]
    return git_lines(args, repo)


def main() -> int:
    parser = argparse.ArgumentParser(description="Create nightly release notes.")
    parser.add_argument("--version", required=True, help="Version or tag (e.g. v1.2.3.4).")
    parser.add_argument("--output", required=True, help="Output markdown file.")
    parser.add_argument(
        "--commit-range",
        help="Optional commit range shown in the notes (e.g. v1.2.3.3..HEAD).",
    )
    parser.add_argument(
        "--include-full-changelog",
        action="store_true",
        help="Append docs/changelog-custom.txt when present.",
    )
    parser.add_argument(
        "--repo",
        default=".",
        help="Repository root (defaults to current directory).",
    )
    args = parser.parse_args()

    repo = Path(args.repo).resolve()
    if not (repo / ".git").exists():
        print(f"Not a git repository: {repo}", file=sys.stderr)
        return 1

    version = args.version.lstrip("vV")
    utc_now = dt.datetime.now(dt.UTC).strftime("%Y-%m-%d %H:%M UTC")
    commits = commit_lines(repo, args.commit_range)

    notes = [
        f"# VibeRadiant Nightly v{version}",
        "",
        f"Generated: {utc_now}",
    ]
    if args.commit_range:
        notes.append(f"Commit range: `{args.commit_range}`")
    notes.extend(["", "## Commits", ""])

    if commits:
        notes.extend(commits)
    else:
        notes.append("- No new commits matched this nightly range.")

    if args.include_full_changelog:
        changelog_path = repo / "docs" / "changelog-custom.txt"
        if changelog_path.exists():
            notes.extend(
                [
                    "",
                    "## Full Changelog Snapshot",
                    "",
                    changelog_path.read_text(encoding="utf-8").rstrip(),
                ]
            )

    out_path = Path(args.output).resolve()
    out_path.write_text("\n".join(notes).rstrip() + "\n", encoding="utf-8")
    print(f"Wrote release notes: {out_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
