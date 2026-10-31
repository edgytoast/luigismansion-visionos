"""A second static HLE registration silently replaces the first at startup."""
from pathlib import Path
import collections
import re

root = Path(__file__).resolve().parents[1]
registrations = collections.defaultdict(list)
for source in sorted((root / "src/recomp").glob("*.cpp")):
    text = source.read_text(encoding="utf-8", errors="replace")
    for match in re.finditer(r"^(?:GX)?HLE\((\w+)\)", text, re.MULTILINE):
        line = text.count("\n", 0, match.start()) + 1
        registrations[match[1]].append(f"{source.name}:{line}")
duplicates = {name: sites for name, sites in registrations.items() if len(sites) > 1}
for name, sites in duplicates.items():
    print(f"Duplicate HLE registration {name}: {', '.join(sites)}")
if duplicates:
    raise SystemExit(1)
print(f"Verified {len(registrations)} unique HLE registrations")
