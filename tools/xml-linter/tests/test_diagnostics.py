"""Tests for diagnostics module."""

from __future__ import annotations

from pathlib import Path

import pytest

from helix_xml_linter.diagnostics import (
    CheckType,
    Diagnostic,
    LintResult,
    Severity,
    format_results_github,
    format_results_json,
    format_results_text,
)


@pytest.fixture
def sample_diagnostic() -> Diagnostic:
    """Return a sample diagnostic for testing."""
    return Diagnostic(
        file=Path("test.xml"),
        line=10,
        column=5,
        severity=Severity.ERROR,
        check=CheckType.UNKNOWN_ATTRIBUTE,
        message="Unknown attribute 'bad_attr' on <lv_label>",
        element="lv_label",
        attribute="bad_attr",
        value="hello",
    )


@pytest.fixture
def sample_warning() -> Diagnostic:
    """Return a sample warning diagnostic."""
    return Diagnostic(
        file=Path("test.xml"),
        line=15,
        column=3,
        severity=Severity.WARNING,
        check=CheckType.UNKNOWN_SUBJECT_REF,
        message="Unknown subject reference: foo",
        element="lv_label",
        attribute="bind_text",
        value="foo",
    )


class TestDiagnosticFormatText:
    """Tests for Diagnostic.format_text()."""

    def test_basic_format(self, sample_diagnostic: Diagnostic) -> None:
        """Test basic text formatting."""
        result = sample_diagnostic.format_text()
        assert "test.xml:10:5" in result
        assert "error:" in result
        assert "Unknown attribute" in result
        assert "[unknown-attribute]" in result

    def test_warning_format(self, sample_warning: Diagnostic) -> None:
        """Test warning text formatting."""
        result = sample_warning.format_text()
        assert "warning:" in result
        assert "[unknown-subject-ref]" in result


class TestDiagnosticFormatJson:
    """Tests for Diagnostic.format_json()."""

    def test_full_json(self, sample_diagnostic: Diagnostic) -> None:
        """Test JSON formatting with all fields."""
        result = sample_diagnostic.format_json()
        assert result["file"] == "test.xml"
        assert result["line"] == 10
        assert result["column"] == 5
        assert result["severity"] == "error"
        assert result["check"] == "unknown-attribute"
        assert result["element"] == "lv_label"
        assert result["attribute"] == "bad_attr"
        assert result["value"] == "hello"

    def test_minimal_json(self) -> None:
        """Test JSON formatting with only required fields."""
        diag = Diagnostic(
            file=Path("test.xml"),
            line=1,
            column=1,
            severity=Severity.INFO,
            check=CheckType.MISSING_REQUIRED,
            message="Missing something",
        )
        result = diag.format_json()
        assert "element" not in result
        assert "attribute" not in result
        assert "value" not in result


class TestDiagnosticFormatGithub:
    """Tests for Diagnostic.format_github()."""

    def test_error_annotation(self, sample_diagnostic: Diagnostic) -> None:
        """Test GitHub Actions error annotation format."""
        result = sample_diagnostic.format_github()
        assert (
            result
            == "::error file=test.xml,line=10,col=5::Unknown attribute 'bad_attr' on <lv_label>"
        )

    def test_warning_annotation(self, sample_warning: Diagnostic) -> None:
        """Test GitHub Actions warning annotation format."""
        result = sample_warning.format_github()
        assert result.startswith("::warning")
        assert "file=test.xml" in result


class TestLintResult:
    """Tests for LintResult computed properties."""

    def test_clean_result(self) -> None:
        """Test is_clean with no diagnostics."""
        result = LintResult(file=Path("clean.xml"))
        assert result.is_clean is True
        assert result.error_count == 0

    def test_error_result(self) -> None:
        """Test is_clean with error diagnostics."""
        diag = Diagnostic(
            file=Path("test.xml"),
            line=1,
            column=1,
            severity=Severity.ERROR,
            check=CheckType.UNKNOWN_WIDGET,
            message="Bad widget",
        )
        result = LintResult(file=Path("test.xml"), diagnostics=[diag])
        assert result.is_clean is False
        assert result.error_count == 1
        assert result.has_warnings is False

    def test_warning_only_result(self) -> None:
        """Test is_clean with only warnings."""
        diag = Diagnostic(
            file=Path("test.xml"),
            line=1,
            column=1,
            severity=Severity.WARNING,
            check=CheckType.UNKNOWN_SUBJECT_REF,
            message="Unknown subject",
        )
        result = LintResult(file=Path("test.xml"), diagnostics=[diag])
        assert result.is_clean is True
        assert result.has_warnings is True
        assert result.warning_count == 1

    def test_mixed_result(self) -> None:
        """Test counts with mixed severities."""
        diags = [
            Diagnostic(
                file=Path("test.xml"),
                line=i,
                column=1,
                severity=sev,
                check=CheckType.UNKNOWN_ATTRIBUTE,
                message=f"Issue {i}",
            )
            for i, sev in enumerate([Severity.ERROR, Severity.WARNING, Severity.INFO])
        ]
        result = LintResult(file=Path("test.xml"), diagnostics=diags)
        assert result.error_count == 1
        assert result.warning_count == 1
        assert result.info_count == 1


class TestFormatResults:
    """Tests for format_results_* functions."""

    def test_text_format_multiple_results(self) -> None:
        """Test text formatting with multiple results."""
        results = [
            LintResult(
                file=Path("a.xml"),
                diagnostics=[
                    Diagnostic(
                        file=Path("a.xml"),
                        line=1,
                        column=1,
                        severity=Severity.ERROR,
                        check=CheckType.UNKNOWN_ATTRIBUTE,
                        message="Bad attr",
                    ),
                ],
            ),
            LintResult(file=Path("b.xml"), diagnostics=[]),
        ]
        text = format_results_text(results)
        assert "a.xml:1:1" in text
        assert "2 file(s) checked" in text
        assert "1 error(s)" in text

    def test_json_format(self) -> None:
        """Test JSON formatting."""
        results = [
            LintResult(
                file=Path("a.xml"),
                diagnostics=[
                    Diagnostic(
                        file=Path("a.xml"),
                        line=1,
                        column=1,
                        severity=Severity.ERROR,
                        check=CheckType.UNKNOWN_WIDGET,
                        message="Unknown widget",
                    ),
                ],
            ),
        ]
        json_output = format_results_json(results)
        assert len(json_output) == 1
        assert json_output[0]["file"] == "a.xml"

    def test_github_format(self) -> None:
        """Test GitHub Actions formatting."""
        results = [
            LintResult(
                file=Path("a.xml"),
                diagnostics=[
                    Diagnostic(
                        file=Path("a.xml"),
                        line=3,
                        column=2,
                        severity=Severity.WARNING,
                        check=CheckType.UNKNOWN_STYLE_REF,
                        message="Unknown style",
                    ),
                ],
            ),
        ]
        gh = format_results_github(results)
        assert "::warning file=a.xml,line=3,col=2::Unknown style" in gh
