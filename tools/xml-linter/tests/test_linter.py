"""Tests for linter module — Tier 1 schema validation."""

from __future__ import annotations

from pathlib import Path

import pytest

from helix_xml_linter.diagnostics import CheckType, Severity
from helix_xml_linter.linter import Linter, LinterConfig
from helix_xml_linter.schema_loader import Schema


class TestLinterCleanFiles:
    """Tests for linting files with no issues."""

    def test_valid_basic_xml(self, schema: Schema, valid_basic_xml: Path) -> None:
        """Test linting a valid XML file produces no errors."""
        linter = Linter(schema, LinterConfig(enable_xref=False))
        result = linter.lint_file(valid_basic_xml)
        errors = [d for d in result.diagnostics if d.severity == Severity.ERROR]
        assert len(errors) == 0, f"Unexpected errors: {[d.message for d in errors]}"

    def test_valid_refs_xml(self, schema: Schema, valid_refs_xml: Path) -> None:
        """Test linting a valid file with subjects and event_cb."""
        linter = Linter(schema, LinterConfig(enable_xref=False))
        result = linter.lint_file(valid_refs_xml)
        errors = [d for d in result.diagnostics if d.severity == Severity.ERROR]
        assert len(errors) == 0, f"Unexpected errors: {[d.message for d in errors]}"

    def test_style_props_xml(self, schema: Schema, style_props_xml: Path) -> None:
        """Test linting a file with valid style properties."""
        linter = Linter(schema, LinterConfig(enable_xref=False))
        result = linter.lint_file(style_props_xml)
        errors = [d for d in result.diagnostics if d.severity == Severity.ERROR]
        assert len(errors) == 0, f"Unexpected errors: {[d.message for d in errors]}"


class TestLinterUnknownWidget:
    """Tests for unknown-widget detection."""

    def test_unknown_widget_detected(self, schema: Schema, unknown_widget_xml: Path) -> None:
        """Test unknown widget types are detected."""
        linter = Linter(schema, LinterConfig(enable_xref=False))
        result = linter.lint_file(unknown_widget_xml)
        widget_errors = [d for d in result.diagnostics if d.check == CheckType.UNKNOWN_WIDGET]
        assert len(widget_errors) >= 2
        tags = {d.element for d in widget_errors}
        assert "lv_unknown_widget" in tags
        assert "lv_imaginary_thing" in tags


class TestLinterUnknownAttribute:
    """Tests for unknown-attribute detection."""

    def test_unknown_attribute_detected(self, schema: Schema, unknown_attr_xml: Path) -> None:
        """Test unknown attributes are detected."""
        linter = Linter(schema, LinterConfig(enable_xref=False))
        result = linter.lint_file(unknown_attr_xml)
        attr_errors = [d for d in result.diagnostics if d.check == CheckType.UNKNOWN_ATTRIBUTE]
        assert len(attr_errors) >= 1
        attr_names = {d.attribute for d in attr_errors}
        assert "unknown_attr" in attr_names

    def test_unknown_style_prop_detected(self, schema: Schema, unknown_attr_xml: Path) -> None:
        """Test unknown style properties are detected."""
        linter = Linter(schema, LinterConfig(enable_xref=False))
        result = linter.lint_file(unknown_attr_xml)
        style_errors = [d for d in result.diagnostics if d.check == CheckType.UNKNOWN_STYLE_PROP]
        assert len(style_errors) >= 1
        assert any("bg_colr" in d.attribute for d in style_errors)


class TestLinterInvalidEnum:
    """Tests for invalid-enum-value detection."""

    def test_invalid_enum_detected(self, schema: Schema, bad_enum_xml: Path) -> None:
        """Test invalid enum values are detected."""
        linter = Linter(schema, LinterConfig(enable_xref=False))
        result = linter.lint_file(bad_enum_xml)
        enum_errors = [d for d in result.diagnostics if d.check == CheckType.INVALID_ENUM_VALUE]
        assert len(enum_errors) >= 2
        values = {d.value for d in enum_errors}
        assert "invalid_mode" in values
        assert "bad_align" in values


class TestLinterInvalidBool:
    """Tests for invalid-bool-value detection."""

    def test_invalid_bool_detected(self, schema: Schema, bad_enum_xml: Path) -> None:
        """Test invalid boolean values are detected."""
        linter = Linter(schema, LinterConfig(enable_xref=False))
        result = linter.lint_file(bad_enum_xml)
        bool_errors = [d for d in result.diagnostics if d.check == CheckType.INVALID_BOOL_VALUE]
        assert len(bool_errors) >= 1
        assert any(d.value == "yes" for d in bool_errors)


class TestLinterMalformedXml:
    """Tests for malformed XML handling."""

    def test_malformed_xml_returns_parse_error(self, schema: Schema, malformed_xml: Path) -> None:
        """Test malformed XML produces parse error diagnostic."""
        linter = Linter(schema, LinterConfig(enable_xref=False))
        result = linter.lint_file(malformed_xml)
        parse_errors = [d for d in result.diagnostics if d.check == CheckType.XML_PARSE_ERROR]
        assert len(parse_errors) >= 1
        assert result.is_clean is False


class TestLinterConfig:
    """Tests for LinterConfig filtering."""

    def test_severity_filter_error_only(self, schema: Schema, bad_enum_xml: Path) -> None:
        """Test min severity error suppresses warnings."""
        config = LinterConfig(min_severity=Severity.ERROR, enable_xref=False)
        linter = Linter(schema, config)
        result = linter.lint_file(bad_enum_xml)
        # Should only have error-severity diagnostics
        assert all(d.severity == Severity.ERROR for d in result.diagnostics)

    def test_severity_filter_info(self, schema: Schema, valid_basic_xml: Path) -> None:
        """Test min severity info includes everything."""
        config = LinterConfig(min_severity=Severity.INFO, enable_xref=False)
        linter = Linter(schema, config)
        result = linter.lint_file(valid_basic_xml)
        # No diagnostics at all for a clean file
        assert len(result.diagnostics) == 0


class TestLinterInheritance:
    """Tests for widget attribute inheritance."""

    def test_inherited_attrs_not_flagged(self, schema: Schema) -> None:
        """Test that inherited attributes from lv_obj are not flagged on child widgets."""
        from helix_xml_linter.xml_parser import ParsedElement

        linter = Linter(schema, LinterConfig(enable_xref=False))

        # lv_label should have lv_obj attrs
        elem = ParsedElement(
            tag="lv_label",
            attributes={"text": "hello", "width": "100", "hidden": "true"},
            line=1,
            column=1,
            source_file=Path("test.xml"),
        )
        diagnostics = linter.lint_element(elem)
        errors = [d for d in diagnostics if d.severity == Severity.ERROR]
        assert len(errors) == 0

    def test_label_only_attr_on_obj_flagged(self, schema: Schema) -> None:
        """Test that label-specific attr on lv_obj is flagged."""
        from helix_xml_linter.xml_parser import ParsedElement

        linter = Linter(schema, LinterConfig(enable_xref=False))

        # lv_obj should NOT have label-specific 'text' attr
        elem = ParsedElement(
            tag="lv_obj",
            attributes={"text": "hello"},
            line=1,
            column=1,
            source_file=Path("test.xml"),
        )
        diagnostics = linter.lint_element(elem)
        unknown = [d for d in diagnostics if d.check == CheckType.UNKNOWN_ATTRIBUTE]
        assert len(unknown) >= 1
        assert unknown[0].attribute == "text"


class TestLinterStyleProperties:
    """Tests for style property validation."""

    def test_valid_style_attr(self, schema: Schema) -> None:
        """Test valid style_* attributes are accepted."""
        from helix_xml_linter.xml_parser import ParsedElement

        linter = Linter(schema, LinterConfig(enable_xref=False))
        elem = ParsedElement(
            tag="lv_obj",
            attributes={"style_bg_color": "red", "style_width": "100"},
            line=1,
            column=1,
            source_file=Path("test.xml"),
        )
        diagnostics = linter.lint_element(elem)
        errors = [d for d in diagnostics if d.severity == Severity.ERROR]
        assert len(errors) == 0

    def test_invalid_style_attr(self, schema: Schema) -> None:
        """Test invalid style_* attributes are flagged."""
        from helix_xml_linter.xml_parser import ParsedElement

        linter = Linter(schema, LinterConfig(enable_xref=False))
        elem = ParsedElement(
            tag="lv_obj",
            attributes={"style_bg_colr": "red"},
            line=1,
            column=1,
            source_file=Path("test.xml"),
        )
        diagnostics = linter.lint_element(elem)
        style_errors = [d for d in diagnostics if d.check == CheckType.UNKNOWN_STYLE_PROP]
        assert len(style_errors) >= 1

    def test_style_enum_validation(self, schema: Schema) -> None:
        """Test style property enum value validation."""
        from helix_xml_linter.xml_parser import ParsedElement

        linter = Linter(schema, LinterConfig(enable_xref=False))
        elem = ParsedElement(
            tag="lv_obj",
            attributes={"style_text_align": "invalid_value"},
            line=1,
            column=1,
            source_file=Path("test.xml"),
        )
        diagnostics = linter.lint_element(elem)
        enum_errors = [d for d in diagnostics if d.check == CheckType.INVALID_ENUM_VALUE]
        assert len(enum_errors) >= 1

    def test_style_const_ref_skipped(self, schema: Schema) -> None:
        """Test style properties with constant references are not validated for enum."""
        from helix_xml_linter.xml_parser import ParsedElement

        linter = Linter(schema, LinterConfig(enable_xref=False))
        elem = ParsedElement(
            tag="lv_obj",
            attributes={"style_pad_all": "#space_md"},
            line=1,
            column=1,
            source_file=Path("test.xml"),
        )
        diagnostics = linter.lint_element(elem)
        errors = [d for d in diagnostics if d.severity == Severity.ERROR]
        assert len(errors) == 0


class TestLinterStyleSelectors:
    """Tests for style property selector syntax (style_<prop>-<selector>).

    LVGL allows selectors like indicator, knob, main, disabled, etc.
    after a '-' in style attribute names.
    """

    @pytest.mark.parametrize(
        "attr_name,attr_value",
        [
            ("style_arc_color-indicator", "#FF0000"),
            ("style_bg_opa-knob", "0"),
            ("style_opa-disabled", "128"),
            ("style_shadow_width-knob", "0"),
            ("style_outline_width-knob", "0"),
            ("style_arc_rounded-indicator", "true"),
            ("style_arc_width-indicator", "2"),
            ("style_arc_opa-main", "255"),
        ],
    )
    def test_valid_style_with_selector(
        self, schema: Schema, attr_name: str, attr_value: str
    ) -> None:
        """Test style properties with valid selector suffixes are accepted."""
        from helix_xml_linter.xml_parser import ParsedElement

        linter = Linter(schema, LinterConfig(enable_xref=False))
        elem = ParsedElement(
            tag="lv_obj",
            attributes={attr_name: attr_value},
            line=1,
            column=1,
            source_file=Path("test.xml"),
        )
        diagnostics = linter.lint_element(elem)
        errors = [d for d in diagnostics if d.severity == Severity.ERROR]
        assert len(errors) == 0, f"Unexpected errors: {[d.message for d in errors]}"

    def test_unknown_selector_warns(self, schema: Schema) -> None:
        """Test unknown selector suffixes produce warnings."""
        from helix_xml_linter.xml_parser import ParsedElement

        linter = Linter(schema, LinterConfig(enable_xref=False))
        elem = ParsedElement(
            tag="lv_obj",
            attributes={"style_bg_opa-unknown_part": "0"},
            line=1,
            column=1,
            source_file=Path("test.xml"),
        )
        diagnostics = linter.lint_element(elem)
        selector_warnings = [
            d for d in diagnostics
            if d.check == CheckType.UNKNOWN_STYLE_PROP and d.severity == Severity.WARNING
        ]
        assert len(selector_warnings) >= 1
        assert "unknown_part" in selector_warnings[0].message

    def test_unknown_prop_with_dash_still_error(self, schema: Schema) -> None:
        """Test unknown base property with selector is still an error."""
        from helix_xml_linter.xml_parser import ParsedElement

        linter = Linter(schema, LinterConfig(enable_xref=False))
        elem = ParsedElement(
            tag="lv_obj",
            attributes={"style_nonexistent_prop-indicator": "5"},
            line=1,
            column=1,
            source_file=Path("test.xml"),
        )
        diagnostics = linter.lint_element(elem)
        style_errors = [d for d in diagnostics if d.check == CheckType.UNKNOWN_STYLE_PROP and d.severity == Severity.ERROR]
        assert len(style_errors) >= 1
        assert "nonexistent_prop" in style_errors[0].message

    @pytest.mark.parametrize(
        "attr_name,expected",
        [
            ("style_arc_width_indicator", "style_arc_width-indicator"),
            ("style_arc_opa_main", "style_arc_opa-main"),
        ],
    )
    def test_selector_underscore_typo_suggests_dash(
        self, schema: Schema, attr_name: str, expected: str
    ) -> None:
        """Selector suffixes accidentally written with '_' get dash suggestions."""
        from helix_xml_linter.xml_parser import ParsedElement

        linter = Linter(schema, LinterConfig(enable_xref=False))
        elem = ParsedElement(
            tag="lv_obj",
            attributes={attr_name: "2"},
            line=1,
            column=1,
            source_file=Path("test.xml"),
        )
        diagnostics = linter.lint_element(elem)
        style_errors = [
            d for d in diagnostics if d.check == CheckType.UNKNOWN_STYLE_PROP
        ]
        assert len(style_errors) == 1
        assert f"Did you mean '{expected}'?" in style_errors[0].message

    def test_selector_underscore_typo_with_state_suggests_dash_and_colon(
        self, schema: Schema
    ) -> None:
        """Internal state syntax is displayed as the original ':' syntax."""
        from helix_xml_linter.xml_parser import ParsedElement

        linter = Linter(schema, LinterConfig(enable_xref=False))
        elem = ParsedElement(
            tag="lv_obj",
            attributes={"style_bg_color_indicator__checked": "#primary"},
            line=1,
            column=1,
            source_file=Path("test.xml"),
        )
        diagnostics = linter.lint_element(elem)
        style_errors = [
            d for d in diagnostics if d.check == CheckType.UNKNOWN_STYLE_PROP
        ]
        assert len(style_errors) == 1
        assert "Did you mean 'style_bg_color-indicator:checked'?" in style_errors[0].message

    def test_selector_underscore_suggestion_requires_valid_base_prop(
        self, schema: Schema
    ) -> None:
        """Do not suggest selector syntax when the base property is still unknown."""
        from helix_xml_linter.xml_parser import ParsedElement

        linter = Linter(schema, LinterConfig(enable_xref=False))
        elem = ParsedElement(
            tag="lv_obj",
            attributes={"style_bg_colr_indicator": "#primary"},
            line=1,
            column=1,
            source_file=Path("test.xml"),
        )
        diagnostics = linter.lint_element(elem)
        style_errors = [
            d for d in diagnostics if d.check == CheckType.UNKNOWN_STYLE_PROP
        ]
        assert len(style_errors) == 1
        assert "Did you mean" not in style_errors[0].message

    def test_style_x_and_y_flagged(self, schema: Schema) -> None:
        """style_x and style_y are NOT exposed by helix-xml's style parser
        (see lib/helix-xml/src/xml/lv_xml_style.c — only style_translate_x and
        style_translate_y are present). Even though they exist in vanilla
        LVGL, our XML wrapper silently drops them. The linter should flag.
        """
        from helix_xml_linter.xml_parser import ParsedElement

        linter = Linter(schema, LinterConfig(enable_xref=False))
        elem = ParsedElement(
            tag="lv_obj",
            attributes={"style_x": "-4", "style_y": "4"},
            line=1,
            column=1,
            source_file=Path("test.xml"),
        )
        diagnostics = linter.lint_element(elem)
        errors = [d for d in diagnostics if d.severity == Severity.ERROR]
        flagged = {d.attribute for d in errors}
        assert flagged == {"style_x", "style_y"}, f"Expected style_x/style_y flagged, got: {flagged}"

    def test_selector_like_valid_property_no_suggestion(self, schema: Schema) -> None:
        """Valid properties ending with selector-like words are not altered."""
        from helix_xml_linter.xml_parser import ParsedElement

        linter = Linter(schema, LinterConfig(enable_xref=False))
        elem = ParsedElement(
            tag="lv_obj",
            attributes={"style_flex_main_place": "center"},
            line=1,
            column=1,
            source_file=Path("test.xml"),
        )
        diagnostics = linter.lint_element(elem)
        assert diagnostics == []


class TestLinterBindAttributes:
    """Tests for bind_* attribute handling."""

    def test_bind_text_accepted(self, schema: Schema) -> None:
        """Test bind_text attribute is accepted without error."""
        from helix_xml_linter.xml_parser import ParsedElement

        linter = Linter(schema, LinterConfig(enable_xref=False))
        elem = ParsedElement(
            tag="lv_label",
            attributes={"bind_text": "my_subject"},
            line=1,
            column=1,
            source_file=Path("test.xml"),
        )
        diagnostics = linter.lint_element(elem)
        errors = [d for d in diagnostics if d.severity == Severity.ERROR]
        assert len(errors) == 0

    def test_bind_value_accepted(self, schema: Schema) -> None:
        """Test bind_value attribute is accepted on slider."""
        from helix_xml_linter.xml_parser import ParsedElement

        linter = Linter(schema, LinterConfig(enable_xref=False))
        elem = ParsedElement(
            tag="lv_slider",
            attributes={"bind_value": "slider_subject"},
            line=1,
            column=1,
            source_file=Path("test.xml"),
        )
        diagnostics = linter.lint_element(elem)
        errors = [d for d in diagnostics if d.severity == Severity.ERROR]
        assert len(errors) == 0

    def test_unprefixed_bind_element_uses_lv_obj_schema(self, schema: Schema) -> None:
        """Runtime lv_obj-* aliases are schema-validated."""
        from helix_xml_linter.xml_parser import ParsedElement

        linter = Linter(schema, LinterConfig(enable_xref=False))
        elem = ParsedElement(
            tag="bind_flag_if_eq",
            attributes={"subject": "runtime_subject", "flag": "hiddden", "ref_value": "1"},
            line=1,
            column=1,
            source_file=Path("test.xml"),
        )
        diagnostics = linter.lint_element(elem)
        enum_errors = [d for d in diagnostics if d.check == CheckType.INVALID_ENUM_VALUE]
        assert len(enum_errors) == 1
        assert enum_errors[0].attribute == "flag"


class TestLinterStateQualifiers:
    """Tests for state qualifier validation in style attributes.

    State-qualified attributes use LVGL syntax like
    ``style_text_color:checked="#text"`` which is preprocessed to
    ``style_text_color__checked="#text"`` before parsing.
    """

    @pytest.mark.parametrize(
        "attr_name,attr_value",
        [
            ("style_text_color__checked", "#text"),
            ("style_bg_opa__checked", "200"),
            ("style_bg_color__checked", "#primary"),
            ("style_border_color__focused", "#primary"),
            ("style_bg_color__hovered", "#primary"),
            ("style_bg_opa__disabled", "100"),
            ("style_text_opa__disabled", "100"),
            ("style_bg_color__pressed", "#primary"),
            ("style_bg_opa__scrolled", "128"),
            ("style_bg_color__default", "#primary"),
        ],
    )
    def test_valid_state_qualifier(
        self, schema: Schema, attr_name: str, attr_value: str
    ) -> None:
        """Test style attributes with valid state qualifiers produce no errors."""
        from helix_xml_linter.xml_parser import ParsedElement

        linter = Linter(schema, LinterConfig(enable_xref=False))
        elem = ParsedElement(
            tag="lv_obj",
            attributes={attr_name: attr_value},
            line=1,
            column=1,
            source_file=Path("test.xml"),
        )
        diagnostics = linter.lint_element(elem)
        errors = [d for d in diagnostics if d.severity == Severity.ERROR]
        assert len(errors) == 0, f"Unexpected errors: {[d.message for d in errors]}"

    def test_invalid_state_qualifier_warns(self, schema: Schema) -> None:
        """Test unknown state qualifier produces a warning."""
        from helix_xml_linter.xml_parser import ParsedElement

        linter = Linter(schema, LinterConfig(enable_xref=False))
        elem = ParsedElement(
            tag="lv_obj",
            attributes={"style_text_color__cheked": "#text"},
            line=1,
            column=1,
            source_file=Path("test.xml"),
        )
        diagnostics = linter.lint_element(elem)
        state_warnings = [
            d for d in diagnostics
            if d.check == CheckType.INVALID_STATE_QUALIFIER
        ]
        assert len(state_warnings) >= 1
        assert "cheked" in state_warnings[0].message

    def test_state_qualifier_display_uses_colon(self, schema: Schema) -> None:
        """Test that diagnostic messages show the original ':' syntax."""
        from helix_xml_linter.xml_parser import ParsedElement

        linter = Linter(schema, LinterConfig(enable_xref=False))
        elem = ParsedElement(
            tag="lv_obj",
            attributes={"style_text_color__cheked": "#text"},
            line=1,
            column=1,
            source_file=Path("test.xml"),
        )
        diagnostics = linter.lint_element(elem)
        state_warnings = [
            d for d in diagnostics
            if d.check == CheckType.INVALID_STATE_QUALIFIER
        ]
        assert len(state_warnings) >= 1
        # Message should show ':cheked' not '__cheked'
        assert ":" in state_warnings[0].message

    def test_combined_part_selector_and_state(self, schema: Schema) -> None:
        """Test combined part selector + state qualifier is valid."""
        from helix_xml_linter.xml_parser import ParsedElement

        linter = Linter(schema, LinterConfig(enable_xref=False))
        elem = ParsedElement(
            tag="lv_obj",
            attributes={
                "style_bg_color-indicator__checked": "#primary",
                "style_bg_opa-knob__pressed": "200",
            },
            line=1,
            column=1,
            source_file=Path("test.xml"),
        )
        diagnostics = linter.lint_element(elem)
        errors = [d for d in diagnostics if d.severity == Severity.ERROR]
        assert len(errors) == 0, f"Unexpected errors: {[d.message for d in errors]}"

    def test_state_qualifier_with_invalid_prop_still_error(self, schema: Schema) -> None:
        """Test that an unknown style prop with valid state is still an error."""
        from helix_xml_linter.xml_parser import ParsedElement

        linter = Linter(schema, LinterConfig(enable_xref=False))
        elem = ParsedElement(
            tag="lv_obj",
            attributes={"style_nonexistent__checked": "100"},
            line=1,
            column=1,
            source_file=Path("test.xml"),
        )
        diagnostics = linter.lint_element(elem)
        prop_errors = [
            d for d in diagnostics
            if d.check == CheckType.UNKNOWN_STYLE_PROP and d.severity == Severity.ERROR
        ]
        assert len(prop_errors) >= 1
        assert "nonexistent" in prop_errors[0].message

    def test_valid_state_qualifier_file(
        self, schema: Schema, state_qualifiers_xml: Path
    ) -> None:
        """Test linting state_qualifiers.xml produces no errors."""
        linter = Linter(schema, LinterConfig(enable_xref=False))
        result = linter.lint_file(state_qualifiers_xml)
        errors = [d for d in result.diagnostics if d.severity == Severity.ERROR]
        assert len(errors) == 0, f"Unexpected errors: {[d.message for d in errors]}"
