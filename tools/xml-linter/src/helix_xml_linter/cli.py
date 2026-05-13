"""CLI entry point for helix-xml-linter."""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

from .crossref import ProjectRegistry
from .diagnostics import (
    LintResult,
    Severity,
    format_results_github,
    format_results_json,
    format_results_text,
)
from .linter import Linter, LinterConfig
from .schema_loader import SchemaLoadError, load_schema


def _find_schema() -> Path | None:
    """Try to auto-find schema.json next to the package or in schema/ dir."""
    # Look in package directory
    package_dir = Path(__file__).parent
    candidate = package_dir.parent.parent / "schema" / "schema.json"
    if candidate.is_file():
        return candidate

    # Look relative to current working directory
    candidate = Path("schema/schema.json")
    if candidate.is_file():
        return candidate.resolve()

    return None


def _collect_xml_paths(paths: list[Path]) -> list[Path]:
    """Expand directories into individual XML file paths."""
    xml_files: list[Path] = []
    for path in paths:
        if path.is_dir():
            xml_files.extend(sorted(path.rglob("*.xml")))
        elif path.is_file():
            xml_files.append(path)
        else:
            print(f"Warning: path not found: {path}", file=sys.stderr)
    return xml_files


def _parse_severity(value: str) -> Severity:
    """Parse a severity string into a Severity enum value."""
    mapping = {
        "error": Severity.ERROR,
        "warning": Severity.WARNING,
        "info": Severity.INFO,
    }
    normalized = value.lower().strip()
    if normalized not in mapping:
        raise ValueError(f"Invalid severity: {value}. Use: error, warning, info")
    return mapping[normalized]


def _create_parser() -> argparse.ArgumentParser:
    """Create the argument parser for the CLI."""
    parser = argparse.ArgumentParser(
        prog="helix-xml-lint",
        description="Schema-driven XML linter for helix-xml (LVGL) files",
    )
    parser.add_argument(
        "files",
        nargs="+",
        type=Path,
        metavar="PATH",
        help="XML files or directories to lint",
    )
    parser.add_argument(
        "-s",
        "--schema",
        type=Path,
        default=None,
        help="Path to schema.json (default: auto-find)",
    )
    parser.add_argument(
        "-f",
        "--format",
        choices=["text", "json", "github"],
        default="text",
        help="Output format (default: text)",
    )
    parser.add_argument(
        "--severity",
        type=str,
        default="info",
        help="Minimum severity to report: error, warning, info (default: info)",
    )
    parser.add_argument(
        "-q",
        "--quiet",
        action="store_true",
        help="Suppress summary output",
    )
    parser.add_argument(
        "-w",
        "--fail-on-warning",
        action="store_true",
        help="Exit with error code when warnings are found",
    )
    parser.add_argument(
        "--no-xref",
        action="store_true",
        help="Disable cross-reference validation",
    )

    return parser


def _create_extract_parser() -> argparse.ArgumentParser:
    """Create parser for the extract-schema subcommand."""
    parser = argparse.ArgumentParser(
        prog="helix-xml-lint extract-schema",
        description="Extract schema from C source files",
    )
    parser.add_argument(
        "source_dir",
        type=Path,
        help="Path to C source directory containing LVGL XML parsers",
    )
    parser.add_argument(
        "-o",
        "--output",
        type=Path,
        default=Path("schema/schema.json"),
        help="Output path for schema.json (default: schema/schema.json)",
    )
    parser.add_argument(
        "--cpp-src",
        type=Path,
        default=None,
        help="Path to src/ui/ containing custom C++ widget files",
    )
    parser.add_argument(
        "--xml-roots",
        type=Path,
        nargs="*",
        default=None,
        help="XML directories to scan for runtime constant names",
    )
    return parser


def main(argv: list[str] | None = None) -> int:
    """Main CLI entry point.

    Returns exit code:
        0 — No errors (clean or warnings only, unless --fail-on-warning)
        1 — Lint errors found in input files
        2 — Tool error (invalid arguments, missing schema, internal error)
    """

    # Detect extract-schema subcommand before main parser
    effective_argv = argv if argv is not None else sys.argv[1:]
    if effective_argv and effective_argv[0] == "extract-schema":
        return _handle_extract_schema_cmd(effective_argv[1:])

    parser = _create_parser()
    args = parser.parse_args(argv)

    # Validate arguments
    try:
        min_severity = _parse_severity(args.severity)
    except ValueError as e:
        print(f"Error: {e}", file=sys.stderr)
        return 2

    # Find or load schema
    schema_path = args.schema
    if schema_path is None:
        schema_path = _find_schema()
    if schema_path is None:
        print(
            "Error: No schema file specified and auto-discovery failed. "
            "Use --schema to specify the path.",
            file=sys.stderr,
        )
        return 2

    # Load schema
    try:
        schema = load_schema(schema_path)
    except SchemaLoadError as e:
        print(f"Error loading schema: {e}", file=sys.stderr)
        return 2

    # Collect XML files
    xml_paths = _collect_xml_paths(args.files)
    if not xml_paths:
        print("No XML files found to lint.", file=sys.stderr)
        return 2

    # Configure linter
    config = LinterConfig(
        fail_on_warning=args.fail_on_warning,
        quiet=args.quiet,
        min_severity=min_severity,
        enable_xref=not args.no_xref,
    )

    # Two-pass linting: build project-wide registry first
    project_registry: ProjectRegistry | None = None
    if len(xml_paths) > 1 and config.enable_xref:
        project_registry = ProjectRegistry.from_files(xml_paths)
        # Register custom component views in the schema so they're recognized
        for comp_name, extends_widget in project_registry.component_view_names.items():
            schema.register_custom_widget(comp_name, extends_widget)

    linter = Linter(schema, config, project_registry=project_registry)

    # Lint all files
    results: list[LintResult] = []
    for xml_path in xml_paths:
        result = linter.lint_file(xml_path)
        results.append(result)

    # Format and output results
    if args.format == "json":
        json_output = format_results_json(results)
        print(json.dumps(json_output, indent=2))
    elif args.format == "github":
        print(format_results_github(results))
    else:
        text_output = format_results_text(results, quiet=args.quiet)
        if text_output.strip():
            print(text_output)

    # Determine exit code
    has_errors = any(not r.is_clean for r in results)
    has_warnings = any(r.has_warnings for r in results)

    if has_errors:
        return 1
    if args.fail_on_warning and has_warnings:
        return 1
    return 0


def _handle_extract_schema_cmd(argv: list[str]) -> int:
    """Handle the extract-schema subcommand.

    Delegates to schema.extract_schema module if available.
    """
    extract_parser = _create_extract_parser()
    args = extract_parser.parse_args(argv)

    source_dir: Path = args.source_dir
    output_path: Path = args.output

    if not source_dir.is_dir():
        print(f"Error: Source directory not found: {source_dir}", file=sys.stderr)
        return 2

    try:
        # Try to import the extraction module
        from schema.extract_schema import (
            extract_cpp_widgets,
            extract_runtime_constants,
            extract_schema,
        )
    except ImportError:
        print(
            "Error: extract-schema subcommand requires the schema extraction module.\n"
            "Ensure schema/extract_schema.py is available.",
            file=sys.stderr,
        )
        return 2

    try:
        schema = extract_schema(source_dir)
        if args.cpp_src and args.cpp_src.is_dir():
            extract_cpp_widgets(args.cpp_src, schema)
        if args.xml_roots:
            runtime_consts = extract_runtime_constants(args.xml_roots)
            if runtime_consts:
                schema["runtime_constants"] = sorted(runtime_consts)
        output_path.parent.mkdir(parents=True, exist_ok=True)
        output_path.write_text(json.dumps(schema, indent=2), encoding="utf-8")
        print(f"Schema extracted to {output_path}")
        return 0
    except Exception as e:
        print(f"Error extracting schema: {e}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
