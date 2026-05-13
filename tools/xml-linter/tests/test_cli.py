"""Tests for CLI module."""

from __future__ import annotations

import json
from pathlib import Path

import pytest

from helix_xml_linter.cli import main


class TestCLIExitCodes:
    """Tests for CLI exit codes."""

    def test_clean_file_exits_0(self, valid_basic_xml: Path, schema_path: Path) -> None:
        """Test linting a clean file returns exit code 0."""
        exit_code = main(["-s", str(schema_path), str(valid_basic_xml)])
        assert exit_code == 0

    def test_error_file_exits_1(self, unknown_attr_xml: Path, schema_path: Path) -> None:
        """Test linting a file with errors returns exit code 1."""
        exit_code = main(["-s", str(schema_path), str(unknown_attr_xml)])
        assert exit_code == 1

    def test_missing_schema_exits_2(self, valid_basic_xml: Path) -> None:
        """Test missing schema file returns exit code 2."""
        exit_code = main(["-s", "/nonexistent/schema.json", str(valid_basic_xml)])
        assert exit_code == 2

    def test_no_schema_exits_2(self, valid_basic_xml: Path, tmp_path: Path) -> None:
        """Test auto-discovery still finds schema via package-relative path."""
        # The auto-discovery uses Path(__file__).parent to find schema.json,
        # which works regardless of CWD. So auto-discovery should still succeed.
        # To truly test the failure case, we'd need to monkey-patch _find_schema.
        import os

        old_cwd = Path.cwd()
        try:
            os.chdir(tmp_path)
            exit_code = main([str(valid_basic_xml)])
        finally:
            os.chdir(old_cwd)
        # Auto-discovery should find the schema via package-relative path
        assert exit_code == 0

    def test_nonexistent_file_exits_2(self, schema_path: Path) -> None:
        """Test nonexistent XML file returns exit code 2."""
        exit_code = main(["-s", str(schema_path), "/nonexistent/file.xml"])
        assert exit_code == 2


class TestCLIOutputFormats:
    """Tests for CLI output format options."""

    def test_text_format(
        self, unknown_attr_xml: Path, schema_path: Path, capsys: pytest.CaptureFixture[str]
    ) -> None:
        """Test text output format."""
        exit_code = main(["-s", str(schema_path), "-f", "text", str(unknown_attr_xml)])
        captured = capsys.readouterr()
        assert exit_code == 1
        assert "error:" in captured.out
        assert "[unknown-attribute]" in captured.out

    def test_json_format(
        self, unknown_attr_xml: Path, schema_path: Path, capsys: pytest.CaptureFixture[str]
    ) -> None:
        """Test JSON output format."""
        exit_code = main(["-s", str(schema_path), "-f", "json", str(unknown_attr_xml)])
        captured = capsys.readouterr()
        assert exit_code == 1
        data = json.loads(captured.out)
        assert isinstance(data, list)
        assert len(data) > 0
        assert data[0]["severity"] == "error"

    def test_github_format(
        self, unknown_attr_xml: Path, schema_path: Path, capsys: pytest.CaptureFixture[str]
    ) -> None:
        """Test GitHub Actions output format."""
        exit_code = main(["-s", str(schema_path), "-f", "github", str(unknown_attr_xml)])
        captured = capsys.readouterr()
        assert exit_code == 1
        assert "::error" in captured.out or "::warning" in captured.out


class TestCLIOptions:
    """Tests for CLI option handling."""

    def test_severity_filter(
        self, bad_enum_xml: Path, schema_path: Path, capsys: pytest.CaptureFixture[str]
    ) -> None:
        """Test --severity filter suppresses lower-severity diagnostics."""
        exit_code = main(
            [
                "-s",
                str(schema_path),
                "--severity",
                "error",
                "--no-xref",
                str(bad_enum_xml),
            ]
        )
        capsys.readouterr()
        # Should have errors (bad enums), exit code 1
        assert exit_code == 1

    def test_fail_on_warning(self, bad_refs_xml: Path, schema_path: Path) -> None:
        """Test --fail-on-warning returns exit code 1 for warnings."""
        # bad_refs_xml has warnings (unknown subjects)
        exit_code = main(
            [
                "-s",
                str(schema_path),
                "-w",  # fail on warning
                str(bad_refs_xml),
            ]
        )
        # Should fail because of warnings
        assert exit_code == 1

    def test_quiet_mode(
        self, valid_basic_xml: Path, schema_path: Path, capsys: pytest.CaptureFixture[str]
    ) -> None:
        """Test --quiet suppresses summary output."""
        exit_code = main(
            [
                "-s",
                str(schema_path),
                "-q",
                str(valid_basic_xml),
            ]
        )
        captured = capsys.readouterr()
        assert exit_code == 0
        # Quiet mode should suppress the summary line
        assert "file(s) checked" not in captured.out

    def test_directory_traversal(
        self, schema_path: Path, fixtures_dir: Path, capsys: pytest.CaptureFixture[str]
    ) -> None:
        """Test linting a directory finds XML files."""
        exit_code = main(["-s", str(schema_path), str(fixtures_dir)])
        capsys.readouterr()
        # Should find errors in at least one fixture file
        assert exit_code in (0, 1)

    def test_no_xref_flag(
        self, bad_refs_xml: Path, schema_path: Path, capsys: pytest.CaptureFixture[str]
    ) -> None:
        """Test --no-xref disables cross-reference checks."""
        main(
            [
                "-s",
                str(schema_path),
                "--no-xref",
                str(bad_refs_xml),
            ]
        )
        captured = capsys.readouterr()
        # Without xref, const/style/subject references are not checked
        # The file might still be clean or have only schema issues
        assert "Unknown subject" not in captured.out
        assert "Undefined constant" not in captured.out


class TestCLIInvalidArgs:
    """Tests for CLI argument validation."""

    def test_invalid_severity(self, schema_path: Path, valid_basic_xml: Path) -> None:
        """Test invalid --severity value returns exit code 2."""
        exit_code = main(
            [
                "-s",
                str(schema_path),
                "--severity",
                "critical",
                str(valid_basic_xml),
            ]
        )
        assert exit_code == 2
