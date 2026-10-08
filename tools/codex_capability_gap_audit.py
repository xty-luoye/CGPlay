import argparse
import json
from datetime import datetime, timezone
from pathlib import Path


def main() -> int:
    parser = argparse.ArgumentParser(description="Compare the captured Codex app-server protocol with CGPlay integration references.")
    parser.add_argument("--root", default=str(Path(__file__).resolve().parents[1]))
    parser.add_argument("--protocol", help="Protocol method snapshot or a previous capability audit containing a methods array.")
    parser.add_argument("--output", required=True)
    args = parser.parse_args()

    root = Path(args.root).resolve()
    protocol_path = Path(args.protocol).resolve() if args.protocol else root / "tests/artifacts/codex_full_local_20260713/protocol_method_audit.json"
    if not protocol_path.exists():
        candidates = sorted(
            root.glob("tests/artifacts/**/codex_capability*.json"),
            key=lambda path: path.stat().st_mtime,
            reverse=True,
        )
        protocol_path = next((path for path in candidates if path.resolve() != Path(args.output).resolve()), protocol_path)
    source_paths = [
        root / "src/plugins/codex/CodexPlugin.cpp",
        root / "src/plugins/codex/CodexPlugin.h",
        root / "src/plugins/codex/CodexAgentWorkspace.cpp",
        root / "src/plugins/codex/CodexAgentWorkspace.h",
    ]
    if not protocol_path.exists():
        parser.error("protocol snapshot not found; pass --protocol with a captured method snapshot")
    protocol = json.loads(protocol_path.read_text(encoding="utf-8-sig"))
    source = "\n".join(path.read_text(encoding="utf-8", errors="replace") for path in source_paths)
    # The official-method picker is a discoverability catalog, not an implementation.
    # Exclude it so the report continues to distinguish callable shortcuts from real handlers.
    catalog_start = source.find("const QStringList officialMethods")
    if catalog_start >= 0:
        catalog_end = source.find("};", catalog_start)
        if catalog_end >= 0:
            source = source[:catalog_start] + source[catalog_end + 2:]
    methods = protocol.get("methods", [])
    if methods and isinstance(methods[0], dict):
        methods = [row.get("method", "") for row in methods]
    methods = [method for method in methods if isinstance(method, str) and method]
    rows = [{"method": method, "referenced": method in source} for method in methods]
    report = {
        "generatedAt": datetime.now(timezone.utc).isoformat(),
        "protocolSource": str(protocol_path),
        "methodCount": len(rows),
        "referencedCount": sum(row["referenced"] for row in rows),
        "unreferencedCount": sum(not row["referenced"] for row in rows),
        "unreferencedMethods": [row["method"] for row in rows if not row["referenced"]],
        "methods": rows,
        "note": "A referenced method is not automatically feature-complete; use the companion capability matrix for UI and runtime equivalence.",
    }
    output = Path(args.output)
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8")
    print(json.dumps({key: report[key] for key in ("methodCount", "referencedCount", "unreferencedCount")}, ensure_ascii=False))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
