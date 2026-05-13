"""Tests for xml_parser module."""

from __future__ import annotations

from pathlib import Path

from helix_xml_linter.xml_parser import ParsedElement, parse_xml_file


class TestParseXmlFile:
    """Tests for parse_xml_file()."""

    def test_parse_valid_file(self, valid_basic_xml: Path) -> None:
        """Test parsing a valid XML file returns elements."""
        elements, diagnostics = parse_xml_file(valid_basic_xml)
        assert len(diagnostics) == 0
        assert len(elements) > 0

    def test_parse_finds_component(self, valid_basic_xml: Path) -> None:
        """Test parsing finds the root component element."""
        elements, _ = parse_xml_file(valid_basic_xml)
        tags = [e.tag for e in elements]
        assert "component" in tags

    def test_parse_finds_view(self, valid_basic_xml: Path) -> None:
        """Test parsing finds view elements."""
        elements, _ = parse_xml_file(valid_basic_xml)
        tags = [e.tag for e in elements]
        assert "view" in tags

    def test_parse_finds_widgets(self, valid_basic_xml: Path) -> None:
        """Test parsing finds widget elements."""
        elements, _ = parse_xml_file(valid_basic_xml)
        tags = [e.tag for e in elements]
        assert "lv_obj" in tags
        assert "lv_label" in tags
        assert "lv_button" in tags

    def test_parse_extracts_attributes(self, valid_basic_xml: Path) -> None:
        """Test parsing extracts element attributes."""
        elements, _ = parse_xml_file(valid_basic_xml)
        labels = [e for e in elements if e.tag == "lv_label"]
        assert len(labels) >= 2
        # First label should have text attribute
        text_labels = [lbl for lbl in labels if "text" in lbl.attributes]
        assert len(text_labels) >= 1

    def test_parse_tracks_source_file(self, valid_basic_xml: Path) -> None:
        """Test parsed elements have source file set."""
        elements, _ = parse_xml_file(valid_basic_xml)
        assert all(e.source_file == valid_basic_xml for e in elements)

    def test_parse_tracks_line_numbers(self, valid_basic_xml: Path) -> None:
        """Test parsed elements have line numbers > 0."""
        elements, _ = parse_xml_file(valid_basic_xml)
        assert all(e.line > 0 for e in elements)

    def test_repeated_tags_get_distinct_line_numbers(self, tmp_path: Path) -> None:
        """Repeated element tags should report their own source lines."""
        xml = tmp_path / "repeated.xml"
        xml.write_text(
            "<component>\n"
            "  <view extends=\"lv_obj\">\n"
            "    <lv_label text=\"first\"/>\n"
            "    <lv_label text=\"second\"/>\n"
            "  </view>\n"
            "</component>\n",
            encoding="utf-8",
        )

        elements, diagnostics = parse_xml_file(xml)
        assert diagnostics == []
        labels = [elem for elem in elements if elem.tag == "lv_label"]
        assert [label.line for label in labels] == [3, 4]

    def test_parse_tracks_parent_tag(self, valid_basic_xml: Path) -> None:
        """Test parsed elements have parent tags set."""
        elements, _ = parse_xml_file(valid_basic_xml)
        # Root element has empty parent tag
        root = elements[0]
        assert root.parent_tag == ""
        # Child elements should have parent tags
        children = [e for e in elements if e.parent_tag]
        assert len(children) > 0

    def test_parse_malformed_xml(self, malformed_xml: Path) -> None:
        """Test parsing malformed XML returns error diagnostic."""
        _elements, diagnostics = parse_xml_file(malformed_xml)
        assert len(diagnostics) > 0
        assert diagnostics[0].check.value == "xml-parse-error"

    def test_parse_nonexistent_file(self) -> None:
        """Test parsing nonexistent file returns error."""
        _elements, diagnostics = parse_xml_file(Path("/nonexistent/file.xml"))
        assert len(diagnostics) > 0
        assert diagnostics[0].check.value == "xml-parse-error"
        assert "not found" in diagnostics[0].message.lower()

    def test_parse_preserves_children(self, valid_basic_xml: Path) -> None:
        """Test that the element tree structure is preserved."""
        elements, _ = parse_xml_file(valid_basic_xml)
        # Root element should have children
        root = elements[0]
        assert len(root.children) > 0

    def test_parse_const_elements(self, valid_basic_xml: Path) -> None:
        """Test parsing finds constant definition elements."""
        elements, _ = parse_xml_file(valid_basic_xml)
        px_elements = [e for e in elements if e.tag == "px"]
        assert len(px_elements) >= 1
        assert "name" in px_elements[0].attributes
        assert "value" in px_elements[0].attributes


class TestParsedElement:
    """Tests for ParsedElement dataclass."""

    def test_is_structural_component(self) -> None:
        """Test component tag is structural."""
        elem = ParsedElement(tag="component", attributes={}, line=1, column=1)
        assert elem.is_structural is True

    def test_is_structural_view(self) -> None:
        """Test view tag is structural."""
        elem = ParsedElement(tag="view", attributes={}, line=1, column=1)
        assert elem.is_structural is True

    def test_is_not_structural_widget(self) -> None:
        """Test widget tag is not structural."""
        elem = ParsedElement(tag="lv_obj", attributes={}, line=1, column=1)
        assert elem.is_structural is False

    def test_is_const_definition(self) -> None:
        """Test element inside consts is a const definition."""
        elem = ParsedElement(tag="px", attributes={}, line=1, column=1, parent_tag="consts")
        assert elem.is_const_definition is True

    def test_is_not_const_definition(self) -> None:
        """Test element outside consts is not a const definition."""
        elem = ParsedElement(tag="px", attributes={}, line=1, column=1, parent_tag="view")
        assert elem.is_const_definition is False

    def test_is_subject_definition(self) -> None:
        """Test subject element inside subjects is a definition."""
        elem = ParsedElement(tag="subject", attributes={}, line=1, column=1, parent_tag="subjects")
        assert elem.is_subject_definition is True


class TestStateQualifierPreprocessing:
    """Tests for the ':' → '__' regex preprocessor in xml_parser."""

    def test_state_qualified_xml_parses(self, state_qualifiers_xml: Path) -> None:
        """Test that XML with state-qualified attributes parses without error."""
        elements, diagnostics = parse_xml_file(state_qualifiers_xml)
        parse_errors = [d for d in diagnostics if d.check.value == "xml-parse-error"]
        assert len(parse_errors) == 0, f"Unexpected parse errors: {[d.message for d in parse_errors]}"
        assert len(elements) > 0

    def test_state_qualifiers_converted_to_double_underscore(
        self, state_qualifiers_xml: Path
       ) -> None:
        """Test that ':' in state-qualified attribute names becomes '__'."""
        elements, _ = parse_xml_file(state_qualifiers_xml)
        all_attrs: dict[str, str] = {}
        for elem in elements:
            all_attrs.update(elem.attributes)

        # Original XML has style_text_color:checked — should appear as style_text_color__checked
        assert "style_text_color__checked" in all_attrs

    def test_normal_attributes_unchanged(self, state_qualifiers_xml: Path) -> None:
        """Test that normal attributes without ':' are unchanged."""
        elements, _ = parse_xml_file(state_qualifiers_xml)
        all_attrs: dict[str, str] = {}
        for elem in elements:
            all_attrs.update(elem.attributes)

        # Normal attributes should be preserved
        assert "text" in all_attrs
        assert "width" in all_attrs

    def test_combined_part_and_state_selector(
        self, state_qualifiers_xml: Path
       ) -> None:
        """Test combined part selector + state qualifier: style_bg_color-indicator__checked."""
        elements, _ = parse_xml_file(state_qualifiers_xml)
        all_attrs: dict[str, str] = {}
        for elem in elements:
            all_attrs.update(elem.attributes)

        assert "style_bg_color-indicator__checked" in all_attrs
        assert "style_bg_opa-knob__pressed" in all_attrs

    def test_preprocess_preserves_normal_xml(self, valid_basic_xml: Path) -> None:
        """Test that preprocessing doesn't break normal XML files."""
        elements_before, diag_before = parse_xml_file(valid_basic_xml)
        # Re-read and compare — should still parse cleanly
        assert len(diag_before) == 0
        assert len(elements_before) > 0
