"""Tests for schema_loader module."""

from __future__ import annotations

import json
from pathlib import Path

import pytest

from helix_xml_linter.schema_loader import (
    Schema,
    SchemaLoadError,
    load_schema,
)


class TestLoadSchema:
    """Tests for load_schema()."""

    def test_load_valid_schema(self, schema: Schema) -> None:
        """Test loading a valid schema file."""
        assert "lv_obj" in schema.widgets
        assert "lv_label" in schema.widgets
        assert "align" in schema.enums
        assert "width" in schema.style_properties
        assert "lv_obj" in schema.registered_widgets
        # special_elements contains registered sub-widgets (lv_obj-*, lv_chart-*, etc.)
        assert len(schema.special_elements) > 0

    def test_load_nonexistent_file(self) -> None:
        """Test loading a nonexistent file raises SchemaLoadError."""
        with pytest.raises(SchemaLoadError, match="Schema file not found"):
            load_schema(Path("/nonexistent/schema.json"))

    def test_load_invalid_json(self, tmp_path: Path) -> None:
        """Test loading invalid JSON raises SchemaLoadError."""
        bad_json = tmp_path / "bad.json"
        bad_json.write_text("{invalid json", encoding="utf-8")
        with pytest.raises(SchemaLoadError, match="Invalid JSON"):
            load_schema(bad_json)

    def test_load_malformed_schema(self, tmp_path: Path) -> None:
        """Test loading schema with wrong types raises SchemaLoadError."""
        bad_schema = tmp_path / "bad_schema.json"
        bad_schema.write_text(json.dumps({"widgets": "not a dict"}), encoding="utf-8")
        with pytest.raises(SchemaLoadError, match="widgets"):
            load_schema(bad_schema)

    def test_load_schema_with_missing_fields(self, tmp_path: Path) -> None:
        """Test loading a minimal schema with missing optional fields."""
        minimal = {
            "widgets": {"lv_obj": {"attributes": {}, "inherits": None}},
            "enums": {},
            "style_properties": {},
            "registered_widgets": ["lv_obj"],
            "special_elements": [],
        }
        schema_file = tmp_path / "minimal.json"
        schema_file.write_text(json.dumps(minimal), encoding="utf-8")
        schema = load_schema(schema_file)
        assert "lv_obj" in schema.widgets
        assert len(schema.enums) == 0


class TestSchemaGetValidAttributes:
    """Tests for Schema.get_valid_attributes()."""

    def test_base_widget_attrs(self, schema: Schema) -> None:
        """Test lv_obj has its own attributes."""
        attrs = schema.get_valid_attributes("lv_obj")
        assert "width" in attrs
        assert "height" in attrs
        assert "hidden" in attrs
        assert "align" in attrs

    def test_inherited_attrs(self, schema: Schema) -> None:
        """Test lv_label inherits lv_obj attributes plus its own."""
        attrs = schema.get_valid_attributes("lv_label")
        # Label-specific
        assert "text" in attrs
        assert "long_mode" in attrs
        # Inherited from lv_obj
        assert "width" in attrs
        assert "height" in attrs
        assert "hidden" in attrs

    def test_unknown_widget_returns_empty(self, schema: Schema) -> None:
        """Test unknown widget returns empty attribute set."""
        attrs = schema.get_valid_attributes("lv_nonexistent")
        assert len(attrs) == 0

    def test_caching(self, schema: Schema) -> None:
        """Test that attribute resolution is cached."""
        attrs1 = schema.get_valid_attributes("lv_label")
        attrs2 = schema.get_valid_attributes("lv_label")
        assert attrs1 is attrs2  # Same object from cache


class TestSchemaGetEnumValues:
    """Tests for Schema.get_enum_values()."""

    def test_known_enum(self, schema: Schema) -> None:
        """Test getting values for a known enum."""
        values = schema.get_enum_values("align")
        assert "center" in values
        assert "top_left" in values

    def test_unknown_enum(self, schema: Schema) -> None:
        """Test getting values for unknown enum returns empty list."""
        values = schema.get_enum_values("nonexistent")
        assert values == []


class TestSchemaIsValidWidget:
    """Tests for Schema.is_valid_widget()."""

    def test_registered_widget(self, schema: Schema) -> None:
        """Test known registered widget is valid."""
        assert schema.is_valid_widget("lv_obj") is True
        assert schema.is_valid_widget("lv_label") is True
        assert schema.is_valid_widget("lv_button") is True

    def test_special_element(self, schema: Schema) -> None:
        """Test special elements from schema are valid (lv_obj-* sub-elements)."""
        assert schema.is_valid_widget("lv_obj-style") is True
        assert schema.is_valid_widget("lv_obj-event_cb") is True
        assert schema.is_valid_widget("lv_chart-series") is True
        # Structural tags (component, view) are NOT in schema special_elements
        # They are handled internally by the linter
        assert schema.is_valid_widget("component") is False

    def test_unknown_tag(self, schema: Schema) -> None:
        """Test unknown tag is not valid."""
        assert schema.is_valid_widget("lv_imaginary") is False
        assert schema.is_valid_widget("not_a_widget") is False


class TestSchemaStyleProperties:
    """Tests for Schema style property methods."""

    def test_valid_style_property(self, schema: Schema) -> None:
        """Test known style property is valid."""
        assert schema.is_valid_style_property("width") is True
        assert schema.is_valid_style_property("bg_color") is True
        assert schema.is_valid_style_property("radius") is True

    def test_invalid_style_property(self, schema: Schema) -> None:
        """Test unknown style property is invalid."""
        assert schema.is_valid_style_property("bg_colr") is False
        assert schema.is_valid_style_property("nonsense") is False

    def test_style_property_schema(self, schema: Schema) -> None:
        """Test getting schema for a style property."""
        prop_schema = schema.get_style_property_schema("bg_color")
        assert prop_schema is not None
        assert prop_schema.type == "color"

    def test_style_property_schema_enum(self, schema: Schema) -> None:
        """Test style property with enum type."""
        prop_schema = schema.get_style_property_schema("text_align")
        assert prop_schema is not None
        assert prop_schema.enum is not None


class TestSchemaGetAttributeSchema:
    """Tests for Schema.get_attribute_schema()."""

    def test_direct_attribute(self, schema: Schema) -> None:
        """Test getting schema for a directly owned attribute."""
        attr_schema = schema.get_attribute_schema("lv_label", "text")
        assert attr_schema is not None
        assert attr_schema.type == "string"

    def test_inherited_attribute(self, schema: Schema) -> None:
        """Test getting schema for an inherited attribute."""
        attr_schema = schema.get_attribute_schema("lv_label", "hidden")
        assert attr_schema is not None
        assert attr_schema.type == "bool"

    def test_enum_attribute(self, schema: Schema) -> None:
        """Test getting schema for an enum attribute."""
        attr_schema = schema.get_attribute_schema("lv_label", "long_mode")
        assert attr_schema is not None
        assert attr_schema.type == "enum"
        # The real schema uses 'label_long_mode' as the enum name
        assert attr_schema.enum is not None
        assert "long_mode" in attr_schema.enum

    def test_nonexistent_attribute(self, schema: Schema) -> None:
        """Test getting schema for a nonexistent attribute returns None."""
        attr_schema = schema.get_attribute_schema("lv_label", "nonexistent")
        assert attr_schema is None
