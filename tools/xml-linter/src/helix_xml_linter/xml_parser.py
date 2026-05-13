"""XML parser — parses helix-xml files into structured elements with line tracking."""

from __future__ import annotations

import re
from dataclasses import dataclass, field
from pathlib import Path
from xml.etree.ElementTree import ParseError as ETParseError

from .constants import STRUCTURAL_TAGS
from .diagnostics import CheckType, Diagnostic, Severity


@dataclass
class ParsedElement:
    """A parsed XML element with source location information."""

    tag: str
    attributes: dict[str, str]
    line: int
    column: int
    parent_tag: str = ""
    parent_line: int = 0
    children: list[ParsedElement] = field(default_factory=list)
    source_file: Path = field(default_factory=lambda: Path())

    @property
    def is_structural(self) -> bool:
        """Return True if this is a structural element (component, view, etc.)."""
        return self.tag in STRUCTURAL_TAGS

    @property
    def is_const_definition(self) -> bool:
        """Return True if this element defines a constant (inside <consts>)."""
        return self.parent_tag == "consts"

    @property
    def is_style_block(self) -> bool:
        """Return True if this is a style block definition inside <styles>."""
        return self.parent_tag == "styles"

    @property
    def is_subject_definition(self) -> bool:
        """Return True if this is a subject definition inside <subjects>."""
        return self.tag == "subject" and self.parent_tag == "subjects"


def _compute_line_map(text: str) -> list[int]:
    """Build a mapping from character offset to line number (1-indexed).

    Returns a list where index i is the line number for character offset i.
    """
    line_starts = [0]
    for i, ch in enumerate(text):
        if ch == "\n":
            line_starts.append(i + 1)
    return line_starts


def _offset_to_line_col(line_starts: list[int], offset: int) -> tuple[int, int]:
    """Convert a character offset to (line, column) using the line-start map.

    Both line and column are 1-indexed.
    """
    # Binary search for the line
    lo, hi = 0, len(line_starts) - 1
    while lo < hi:
        mid = (lo + hi + 1) // 2
        if line_starts[mid] <= offset:
            lo = mid
        else:
            hi = mid - 1
    line = lo + 1  # 1-indexed
    col = offset - line_starts[lo] + 1  # 1-indexed
    return line, col


# Regex matching state-qualified attribute names: word+":"word+"="
# e.g. style_text_color:checked=  →  style_text_color__checked=
_STATE_QUALIFIER_RE = re.compile(r"(\b\w+):(\w+\s*=\s*[\"'])")
_START_TAG_RE = re.compile(r"<(?![!/?,])([A-Za-z_][\w:.-]*)(?=[\s>/])")


def _preprocess_state_qualifiers(raw_xml: str) -> str:
    """Replace ':' in state-qualified attribute names with '__'.

    LVGL uses attribute syntax like ``style_text_color:checked="#text"`` which
    causes ElementTree to throw "unbound prefix" because it interprets ':' as
    an XML namespace separator. This function converts those colons to '__' so
    the XML parser can handle them, then the linter validates state qualifiers
    separately.

    The regex only matches ``name:state="value"`` or ``name:state='value'``
    patterns inside element tags — it does NOT affect colons inside attribute
    values, element tags, comments, or CDATA sections.
    """
    return _STATE_QUALIFIER_RE.sub(r"\1__\2", raw_xml)


def parse_xml_file(path: Path) -> tuple[list[ParsedElement], list[Diagnostic]]:
    """Parse an XML file and return elements with location info.

    Returns a tuple of (elements, parse_diagnostics). Parse diagnostics
    capture XML syntax errors. On fatal parse errors, returns partial
    results (the elements parsed so far may be empty).
    """
    if not path.is_file():
        diag = Diagnostic(
            file=path,
            line=0,
            column=0,
            severity=Severity.ERROR,
            check=CheckType.XML_PARSE_ERROR,
            message=f"File not found: {path}",
        )
        return [], [diag]

    try:
        raw_text = path.read_text(encoding="utf-8")
    except OSError as e:
        diag = Diagnostic(
            file=path,
            line=0,
            column=0,
            severity=Severity.ERROR,
            check=CheckType.XML_PARSE_ERROR,
            message=f"Cannot read file: {e}",
        )
        return [], [diag]

    # Pre-process: replace ':' in state-qualified attribute names with '__'.
    # LVGL uses syntax like style_text_color:checked="#text" which causes
    # ElementTree to throw "unbound prefix" because it interprets ':' as
    # an XML namespace separator. We only target attribute name patterns,
    # not values, element tags, or colons inside quotes.
    raw_text = _preprocess_state_qualifiers(raw_text)

    line_starts = _compute_line_map(raw_text)
    tag_positions = [
        (m.group(1), *_offset_to_line_col(line_starts, m.start()))
        for m in _START_TAG_RE.finditer(raw_text)
    ]

    # Use iterparse to get elements with sourceline
    import xml.etree.ElementTree as ET

    elements: list[ParsedElement] = []
    diagnostics: list[Diagnostic] = []

    try:
        # We need sourceline, which requires iterparse. But we also need
        # to walk the tree to build parent relationships.
        # Strategy: parse, then walk with a stack.
        root = ET.fromstring(raw_text)
    except ETParseError as e:
        # Try to extract line number from the error message
        line = 0
        col = 0
        match = re.search(r"line (\d+)", str(e))
        if match:
            line = int(match.group(1))
        match = re.search(r"column (\d+)", str(e))
        if match:
            col = int(match.group(1))

        diag = Diagnostic(
            file=path,
            line=line,
            column=col,
            severity=Severity.ERROR,
            check=CheckType.XML_PARSE_ERROR,
            message=f"XML parse error: {e}",
        )
        return [], [diag]

    # Walk the tree and build ParsedElements
    # ET elements don't have sourceline by default, but the C implementation
    # adds it. We'll use a regex-based approach as fallback.
    tag_position_index = 0

    def _get_element_location(elem: ET.Element) -> tuple[int, int]:
        """Get source location for an element using sourceline or regex fallback."""
        nonlocal tag_position_index
        if hasattr(elem, "sourceline") and elem.sourceline is not None:
            return int(elem.sourceline), 1

        # Fallback: consume matching start tags in document order. ElementTree
        # traversal is pre-order, matching XML source order for start tags.
        while tag_position_index < len(tag_positions):
            tag, line, col = tag_positions[tag_position_index]
            tag_position_index += 1
            if tag == elem.tag:
                return line, col

        return 1, 1

    def _walk(elem: ET.Element, parent_tag: str, parent_line: int, depth: int = 0) -> ParsedElement:
        """Recursively walk the XML tree and build ParsedElements."""
        line, col = _get_element_location(elem)

        parsed = ParsedElement(
            tag=elem.tag,
            attributes=dict(elem.attrib),
            line=line,
            column=col,
            parent_tag=parent_tag,
            parent_line=parent_line,
            source_file=path,
        )

        for child in elem:
            child_element = _walk(child, elem.tag, line, depth + 1)
            parsed.children.append(child_element)

        return parsed

    root_element = _walk(root, "", 0)

    # Flatten the tree into a list for easy iteration, keeping the tree structure
    def _flatten(element: ParsedElement) -> list[ParsedElement]:
        """Flatten the element tree into a list (pre-order traversal)."""
        result = [element]
        for child in element.children:
            result.extend(_flatten(child))
        return result

    elements = _flatten(root_element)

    return elements, diagnostics
