import pytest

from cmake_probe import find_executable


@pytest.mark.parametrize("relative", [
    "Debug/probe.exe", "Release/probe.exe", "probe.exe", "probe",
])
def test_find_actual_cmake_output(tmp_path, relative):
    executable = tmp_path / relative
    executable.parent.mkdir(parents=True, exist_ok=True)
    executable.write_bytes(b"probe fixture")
    assert find_executable(tmp_path, "probe") == executable


def test_missing_cmake_output_fails(tmp_path):
    with pytest.raises(AssertionError, match="was not produced"):
        find_executable(tmp_path, "missing")
