"""Schema loader — loads schema.json and builds fast lookup indexes."""

from __future__ import annotations

import json
from dataclasses import dataclass, field
from pathlib import Path


class SchemaLoadError(Exception):
    """Raised when the schema file cannot be loaded or parsed."""


@dataclass
class AttributeSchema:
    """Schema for a single widget attribute."""

    type: str
    enum: str | None = None


@dataclass
class WidgetSchema:
    """Schema for a single widget type."""

    attributes: dict[str, AttributeSchema]
    inherits: str | None = None


@dataclass
class StylePropertySchema:
    """Schema for a style property."""

    type: str
    enum: str | None = None


@dataclass
class Schema:
    """Loaded and indexed schema for linting."""

    widgets: dict[str, WidgetSchema]
    enums: dict[str, list[str]]
    style_properties: dict[str, StylePropertySchema]
    special_elements: set[str]
    registered_widgets: set[str]
    runtime_constants: set[str] = field(default_factory=set)
    subjects: set[str] = field(default_factory=set)

    # Cached resolved attribute sets: widget_name -> {attr_name, ...}
    _resolved_attributes: dict[str, set[str]] = field(default_factory=dict, repr=False, hash=False)

    def get_valid_attributes(self, widget_name: str) -> set[str]:
        """Get all valid attribute names for a widget, including inherited ones.

        Resolves the inheritance chain and caches the result.
        """
        if widget_name in self._resolved_attributes:
            return self._resolved_attributes[widget_name]

        attrs: set[str] = set()
        seen: set[str] = set()
        current: str | None = widget_name

        while current is not None and current not in seen:
            seen.add(current)
            widget = self.widgets.get(current)
            if widget is None:
                break
            attrs.update(widget.attributes.keys())
            current = widget.inherits

        # Also add bind_* and style_* as valid prefixes (handled separately)
        self._resolved_attributes[widget_name] = attrs
        return attrs

    def get_attribute_schema(self, widget_name: str, attr_name: str) -> AttributeSchema | None:
        """Get the schema for a specific attribute on a widget, following inheritance."""
        seen: set[str] = set()
        current: str | None = widget_name

        while current is not None and current not in seen:
            seen.add(current)
            widget = self.widgets.get(current)
            if widget is None:
                break
            if attr_name in widget.attributes:
                return widget.attributes[attr_name]
            current = widget.inherits

        return None

    def get_enum_values(self, enum_name: str) -> list[str]:
        """Get valid values for an enum type. Returns empty list if unknown."""
        return self.enums.get(enum_name, [])

    def is_valid_widget(self, tag: str) -> bool:
        """Check if a tag is a registered widget, special element, or structural tag."""
        return tag in self.registered_widgets or tag in self.special_elements

    def is_valid_style_property(self, prop: str) -> bool:
        """Check if a style property name is recognized."""
        return prop in self.style_properties

    def get_style_property_schema(self, prop: str) -> StylePropertySchema | None:
        """Get the schema for a style property."""
        return self.style_properties.get(prop)

    def register_custom_widget(self, name: str, extends: str) -> None:
        """Register a custom component view as a valid widget.

        The custom widget inherits all attributes from its extends base.
        """
        self.registered_widgets.add(name)

        # If the extends widget is known in the schema, create a WidgetSchema
        # that inherits from it so attribute validation works correctly.
        if name not in self.widgets and extends in self.widgets:
            self.widgets[name] = WidgetSchema(attributes={}, inherits=extends)

        # Clear any cached resolved attributes for this widget
        self._resolved_attributes.pop(name, None)

    def is_valid_constant(self, name: str) -> bool:
        """Check if a constant name is in the runtime constants set."""
        return name in self.runtime_constants

    def is_valid_subject(self, name: str) -> bool:
        """Check if a subject name is registered in the schema."""
        return name in self.subjects


def load_schema(path: Path) -> Schema:
    """Load and parse a schema.json file into a Schema object.

    Raises SchemaLoadError if the file cannot be read or is malformed.
    """
    if not path.is_file():
        raise SchemaLoadError(f"Schema file not found: {path}")

    try:
        raw = json.loads(path.read_text(encoding="utf-8"))
    except json.JSONDecodeError as e:
        raise SchemaLoadError(f"Invalid JSON in schema file {path}: {e}") from e
    except OSError as e:
        raise SchemaLoadError(f"Cannot read schema file {path}: {e}") from e

    try:
        # Parse widgets
        widgets: dict[str, WidgetSchema] = {}
        raw_widgets = raw.get("widgets", {})
        if not isinstance(raw_widgets, dict):
            raise SchemaLoadError("'widgets' must be a dictionary")
        for name, wdata in raw_widgets.items():
            attrs: dict[str, AttributeSchema] = {}
            for attr_name, adef in wdata.get("attributes", {}).items():
                attrs[attr_name] = AttributeSchema(
                    type=adef.get("type", "string"),
                    enum=adef.get("enum"),
                )
            widgets[name] = WidgetSchema(
                attributes=attrs,
                inherits=wdata.get("inherits"),
            )

        # Parse enums
        enums: dict[str, list[str]] = {}
        raw_enums = raw.get("enums", {})
        if not isinstance(raw_enums, dict):
            raise SchemaLoadError("'enums' must be a dictionary")
        for ename, values in raw_enums.items():
            if not isinstance(values, list):
                raise SchemaLoadError(f"Enum '{ename}' values must be a list")
            enums[ename] = values

        # Parse style properties
        style_properties: dict[str, StylePropertySchema] = {}
        raw_styles = raw.get("style_properties", {})
        if not isinstance(raw_styles, dict):
            raise SchemaLoadError("'style_properties' must be a dictionary")
        for prop_name, pdef in raw_styles.items():
            style_properties[prop_name] = StylePropertySchema(
                type=pdef.get("type", "string"),
                enum=pdef.get("enum"),
            )

        # Parse registered widgets and special elements
        registered = set(raw.get("registered_widgets", []))
        special = set(raw.get("special_elements", []))

        # Parse runtime constants
        runtime_constants = set(raw.get("runtime_constants", []))

        # Parse subjects
        subjects = set(raw.get("subjects", []))

        return Schema(
            widgets=widgets,
            enums=enums,
            style_properties=style_properties,
            special_elements=special,
            registered_widgets=registered,
            runtime_constants=runtime_constants,
            subjects=subjects,
        )
    except (KeyError, TypeError, AttributeError) as e:
        raise SchemaLoadError(f"Malformed schema file {path}: {e}") from e
