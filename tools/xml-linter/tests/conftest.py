"""Shared test fixtures for helix-xml-linter test suite."""

from __future__ import annotations

from pathlib import Path

import pytest

from helix_xml_linter.schema_loader import Schema, load_schema

FIXTURES_DIR = Path(__file__).parent / "fixtures"
SCHEMA_PATH = Path(__file__).parent.parent / "schema" / "schema.json"


@pytest.fixture
def schema_path() -> Path:
    """Return the path to the test schema.json."""
    return SCHEMA_PATH


@pytest.fixture
def schema(schema_path: Path) -> Schema:
    """Load and return the test schema."""
    return load_schema(schema_path)


@pytest.fixture
def fixtures_dir() -> Path:
    """Return the path to the test fixtures directory."""
    return FIXTURES_DIR


@pytest.fixture
def valid_basic_xml(fixtures_dir: Path) -> Path:
    """Return path to valid_basic.xml fixture."""
    return fixtures_dir / "valid_basic.xml"


@pytest.fixture
def unknown_attr_xml(fixtures_dir: Path) -> Path:
    """Return path to unknown_attr.xml fixture."""
    return fixtures_dir / "unknown_attr.xml"


@pytest.fixture
def bad_enum_xml(fixtures_dir: Path) -> Path:
    """Return path to bad_enum.xml fixture."""
    return fixtures_dir / "bad_enum.xml"


@pytest.fixture
def unknown_widget_xml(fixtures_dir: Path) -> Path:
    """Return path to unknown_widget.xml fixture."""
    return fixtures_dir / "unknown_widget.xml"


@pytest.fixture
def bad_refs_xml(fixtures_dir: Path) -> Path:
    """Return path to bad_refs.xml fixture."""
    return fixtures_dir / "bad_refs.xml"


@pytest.fixture
def valid_refs_xml(fixtures_dir: Path) -> Path:
    """Return path to valid_refs.xml fixture."""
    return fixtures_dir / "valid_refs.xml"


@pytest.fixture
def malformed_xml(fixtures_dir: Path) -> Path:
    """Return path to malformed.xml fixture."""
    return fixtures_dir / "malformed.xml"


@pytest.fixture
def style_props_xml(fixtures_dir: Path) -> Path:
    """Return path to style_props.xml fixture."""
    return fixtures_dir / "style_props.xml"


@pytest.fixture
def state_qualifiers_xml(fixtures_dir: Path) -> Path:
    """Return path to state_qualifiers.xml fixture."""
    return fixtures_dir / "state_qualifiers.xml"
