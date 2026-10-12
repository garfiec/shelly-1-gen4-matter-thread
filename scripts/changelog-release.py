#!/usr/bin/env python3
"""Turns CHANGELOG.md's "### [Unreleased]" section into a release.

    scripts/changelog-release.py check                  # there is an Unreleased section
    scripts/changelog-release.py cut 2026.10.0 notes.md # rename it, write its text to notes.md
    scripts/changelog-release.py notes 2026.10.0 notes.md

`cut` renames the "### [Unreleased]" heading to "### [2026.10.0] - <today, UTC>"
and writes the section's text, for the release notes. The next change starts a
new "### [Unreleased]" section. `notes` writes an existing release's section.
Used by .github/workflows/release.yml; see docs/RELEASING.md.
"""
import datetime
import os
import re
import sys

PATH = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "CHANGELOG.md")
UNRELEASED = re.compile(r"^### \[Unreleased\][ \t]*$", re.M)


def fail(msg):
    sys.exit(f"changelog-release: {msg}")


def section(text, heading):
    """The body under the heading that matches, up to the next ### or ## heading."""
    m = heading.search(text)
    if not m:
        return None, None
    end = re.compile(r"^##{1,2} ", re.M).search(text, m.end())
    body = text[m.end():end.start() if end else len(text)].strip("\n")
    return m, body


def write_notes(path, body):
    with open(path, "w") as f:
        f.write(body.strip() + "\n")


def main(argv):
    text = open(PATH).read()
    if argv == ["check"]:
        m, body = section(text, UNRELEASED)
        if not m:
            fail('CHANGELOG.md has no "### [Unreleased]" section to release')
        if not body.strip():
            fail('CHANGELOG.md\'s "### [Unreleased]" section is empty')
        print('CHANGELOG.md has an "### [Unreleased]" section')
    elif len(argv) == 3 and argv[0] == "cut":
        version, out = argv[1], argv[2]
        if re.search(rf"^### \[{re.escape(version)}\]", text, re.M):
            fail(f"CHANGELOG.md already has a {version} section")
        m, body = section(text, UNRELEASED)
        if not m or not body.strip():
            fail('CHANGELOG.md has no "### [Unreleased]" section with changes in it')
        date = datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%d")
        text = text[:m.start()] + f"### [{version}] - {date}" + text[m.end():]
        open(PATH, "w").write(text)
        write_notes(out, body)
        print(f"CHANGELOG.md: [Unreleased] is now [{version}] - {date}")
    elif len(argv) == 3 and argv[0] == "notes":
        version, out = argv[1], argv[2]
        m, body = section(text, re.compile(rf"^### \[{re.escape(version)}\].*$", re.M))
        if not m:
            fail(f"CHANGELOG.md has no {version} section")
        write_notes(out, body)
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main(sys.argv[1:])
