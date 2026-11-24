"""Copy Helium's published bang catalog into the commented [bangs] reference."""

import argparse
import json
from pathlib import Path
import re


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("catalog", type=Path, help="Helium's bangs.json")
    parser.add_argument("configs", type=Path, nargs="+", help="Launch config files to update")
    args = parser.parse_args()

    upstream = args.catalog.read_text(encoding="utf-8")
    start = upstream.index("\n[")
    header, data = upstream[:start], upstream[start:]
    # Helium's manifest is JSON with line comments and trailing commas.
    bangs = json.loads(re.sub(r'",(\s*\])', r'"\1', data))
    entries = {}
    for bang in bangs:
        site = bang["s"].rstrip(" ")
        for trigger in bang["ts"]:
            value = f'{bang["u"]}\t{site}'
            if trigger in entries or any(c in trigger for c in "= \t\n"):
                parser.error(f"invalid or duplicate trigger {trigger!r}")
            if "\n" in value or value.count("\t") != 1:
                parser.error(f"invalid template for {trigger!r}")
            entries[trigger] = value

    for path in args.configs:
        config = path.read_text(encoding="utf-8")
        start, end = "# BEGIN HELIUM BANGS\n", "# END HELIUM BANGS\n"
        prefix, marker, tail = config.partition(start)
        if not marker or end not in tail:
            parser.error(f"missing catalog markers in {path}")
        _, _, suffix = tail.partition(end)
        rows = "".join(f"# {trigger}={value}\n" for trigger, value in entries.items())
        license_comments = "\n".join(line.replace("//", "#", 1)
                                     for line in header.splitlines())
        path.write_text(f"{prefix}{start}{rows}{license_comments}\n{end}{suffix}",
                        encoding="utf-8")
        print(f"{path}: {len(entries):,} triggers")


if __name__ == "__main__":
    main()
