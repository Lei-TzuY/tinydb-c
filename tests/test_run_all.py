"""Exercise the real runner against isolated passing and failing suites."""

import os
from pathlib import Path
import shutil
import subprocess
import sys

RUNNER = Path(__file__).with_name("run_all.py")


def run_fixture(tmp_path, suites, without_pytest=False):
    tests = tmp_path / "tests"
    tests.mkdir()
    shutil.copyfile(RUNNER, tests / "run_all.py")
    for name, source in suites.items():
        (tests / name).write_text(source, encoding="utf-8")
    command = [sys.executable]
    environment = os.environ.copy()
    if without_pytest:
        command.append("-S")
        environment.pop("PYTHONPATH", None)
    command.append(str(tests / "run_all.py"))
    result = subprocess.run(command, cwd=tmp_path, env=environment,
                            capture_output=True, text=True, timeout=60)
    return result.returncode, result.stdout + result.stderr


def test_function_failure_is_executed_and_propagated(tmp_path):
    code, output = run_fixture(tmp_path, {
        "test_a_failure.py": "def test_recovery():\n    raise AssertionError('RECOVERY_EXECUTED')\n",
        "test_z_success.py": "print('SCRIPT_EXECUTED')\n",
    })
    assert code != 0, output
    assert "RECOVERY_EXECUTED" in output
    assert "test_z_success.py" in output and "1 passed, 1 failed" in output


def test_function_suite_gets_pytest_fixtures(tmp_path):
    code, output = run_fixture(tmp_path, {
        "test_fixture.py": ("from pathlib import Path\ndef test_compile(tmp_path):\n"
                            "    assert tmp_path.is_dir()\n"
                            "    Path('fixture-ran.txt').write_text('collected')\n"),
    })
    assert code == 0, output
    assert "0 script suites, 1 pytest suites" in output
    assert "1 passed, 0 failed" in output
    assert (tmp_path / "fixture-ran.txt").read_text() == "collected"


def test_guarded_script_runs_once(tmp_path):
    code, output = run_fixture(tmp_path, {
        "test_guarded.py": r"""from pathlib import Path
def test_contract():
    counter = Path('counter.txt')
    counter.write_text(counter.read_text() + 'called\n' if counter.exists() else 'called\n')
if __name__ == '__main__':
    test_contract()
""",
    })
    assert code == 0, output
    assert "1 script suites, 0 pytest suites" in output
    assert (tmp_path / "counter.txt").read_text() == "called\n"


def test_script_failure_still_propagates(tmp_path):
    code, output = run_fixture(tmp_path, {
        "test_script.py": "raise AssertionError('SCRIPT_FAILURE')\n",
    })
    assert code != 0 and "SCRIPT_FAILURE" in output
    assert "0 passed, 1 failed" in output


def test_syntax_error_is_a_failed_suite(tmp_path):
    code, output = run_fixture(tmp_path, {"test_syntax.py": "def broken(:\n"})
    assert code != 0 and "SyntaxError" in output
    assert "0 passed, 1 failed" in output


def test_collection_error_is_a_failed_suite(tmp_path):
    code, output = run_fixture(tmp_path, {
        "test_import.py": "raise RuntimeError('COLLECTION_FAILURE')\ndef test_case():\n    pass\n",
    })
    assert code != 0 and "COLLECTION_FAILURE" in output
    assert "0 passed, 1 failed" in output


def test_empty_pytest_collection_is_a_failed_suite(tmp_path):
    code, output = run_fixture(tmp_path, {
        "test_uncollected.py": "def test_case():\n    pass\ntest_case.__test__ = False\n",
    })
    assert code != 0 and "no tests ran" in output
    assert "0 passed, 1 failed" in output


def test_missing_pytest_fails_with_install_instruction(tmp_path):
    code, output = run_fixture(tmp_path, {
        "test_case.py": "def test_case():\n    pass\n",
    }, without_pytest=True)
    assert code != 0 and "python -m pip install -r tests/requirements.txt" in output
    assert "passed" not in output


def test_no_suites_is_an_error(tmp_path):
    code, output = run_fixture(tmp_path, {})
    assert code != 0 and "no Tiny Database test suites found" in output
