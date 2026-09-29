#!/usr/bin/env python3
"""Scan tracked files, staged additions, and eligible new project files; never print values."""

from pathlib import Path
import re
import subprocess
import sys

root = Path(sys.argv[1] if len(sys.argv) > 1 else ".").resolve()
result = subprocess.run(
    [
        "git",
        "-C",
        str(root),
        "ls-files",
        "-z",
        "--cached",
        "--others",
        "--exclude-standard",
    ],
    capture_output=True,
)
if result.returncode == 0:
    candidates = [root / p.decode() for p in result.stdout.split(b"\0") if p]
else:
    candidates = [
        p
        for p in root.rglob("*")
        if p.is_file()
        and not any(
            part.startswith(("build", ".git", ".codex", ".agents"))
            for part in p.relative_to(root).parts
        )
    ]
patterns = [
    re.compile(rb"-----BEGIN (?:RSA |EC |OPENSSH )?PRIVATE KEY-----"),
    re.compile(rb"\b(?:gh[pousr]_[A-Za-z0-9]{30,}|github_pat_[A-Za-z0-9_]{40,})\b"),
    re.compile(rb"\bAKIA[A-Z0-9]{16}\b"),
    re.compile(rb"\bsk-(?:proj-)?[A-Za-z0-9_-]{32,}\b"),
]
issues = []
for path in sorted(set(candidates)):
    if not path.is_file():
        continue
    relative = path.relative_to(root)
    if (
        path.name == ".env"
        or path.name.startswith(".env.")
        or path.suffix in {".key", ".pem", ".db", ".sqlite", ".sqlite3"}
    ):
        issues.append(f"{relative}: private local file candidate")
        continue
    data = path.read_bytes()
    for pattern in patterns:
        if pattern.search(data):
            issues.append(f"{relative}: potential credential detected")
            break
if issues:
    print("\n".join(issues), file=sys.stderr)
    sys.exit(1)
print(f"Secret scan passed ({len(set(candidates))} project files)")
