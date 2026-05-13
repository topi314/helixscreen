"""Helix XML Linter — Schema-driven XML linter for helix-xml (LVGL) files."""

__version__ = "0.1.0"

from .crossref import ProjectRegistry
from .diagnostics import CheckType, Diagnostic, LintResult, Severity
from .linter import Linter, LinterConfig
from .schema_loader import Schema, SchemaLoadError, load_schema
from .xml_parser import ParsedElement, parse_xml_file

__all__ = [
    "CheckType",
    "Diagnostic",
    "LintResult",
    "Linter",
    "LinterConfig",
    "ParsedElement",
    "ProjectRegistry",
    "Schema",
    "SchemaLoadError",
    "Severity",
    "load_schema",
    "parse_xml_file",
]
