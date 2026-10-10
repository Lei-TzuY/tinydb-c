import ast
import glob
import importlib.util
import os
import shutil
import subprocess
import sys
import time


def ensure_test_executable_compatibility():
    """Make legacy Windows-oriented test paths work with Unix CMake layouts."""
    repo_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    native_exe = os.path.join(repo_root, "build", "tinydb")
    compat_exe = os.path.join(repo_root, "build", "Debug", "tinydb.exe")

    if os.name == "nt" or os.path.exists(compat_exe) or not os.path.exists(native_exe):
        return

    os.makedirs(os.path.dirname(compat_exe), exist_ok=True)
    try:
        os.symlink(native_exe, compat_exe)
    except (OSError, NotImplementedError):
        shutil.copy2(native_exe, compat_exe)


def uses_pytest(test_file):
    """Function suites need collection; guarded scripts keep their entry point."""
    with open(test_file, encoding="utf-8") as source:
        try:
            module = ast.parse(source.read(), filename=test_file)
        except SyntaxError:
            # Let the script process report the syntax error as a failed suite.
            return False

    has_tests = any(
        isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef))
        and node.name.startswith("test_")
        for node in module.body
    )
    for node in module.body:
        if not isinstance(node, ast.If) or not isinstance(node.test, ast.Compare):
            continue
        comparison = node.test
        if len(comparison.ops) != 1 or not isinstance(comparison.ops[0], ast.Eq):
            continue
        left, right = comparison.left, comparison.comparators[0]
        for name, value in [(left, right), (right, left)]:
            if (isinstance(name, ast.Name) and name.id == "__name__"
                    and isinstance(value, ast.Constant) and value.value == "__main__"):
                return False
    return has_tests


def main():
    base_dir = os.path.dirname(os.path.abspath(__file__))
    test_files = sorted(glob.glob(os.path.join(base_dir, "test_*.py")))
    pytest_files = {path for path in test_files if uses_pytest(path)}

    if not test_files:
        print("FAIL: no Tiny Database test suites found")
        sys.exit(1)
    if pytest_files and importlib.util.find_spec("pytest") is None:
        print("FAIL: function suites require pytest; install test dependencies with:")
        print("python -m pip install -r tests/requirements.txt")
        sys.exit(1)

    ensure_test_executable_compatibility()

    print(f"==================================================")
    print(f"Running {len(test_files)} Tiny Database test suites...")
    print(f"{len(test_files) - len(pytest_files)} script suites, "
          f"{len(pytest_files)} pytest suites")
    print(f"==================================================")

    passed = 0
    failed = 0
    start_time = time.time()

    for test_file in test_files:
        name = os.path.basename(test_file)
        print(f"Running {name:<32} ... ", end="", flush=True)
        t0 = time.time()
        command = [sys.executable, test_file]
        if test_file in pytest_files:
            command = [sys.executable, "-m", "pytest", "-q", test_file]
        res = subprocess.run(command, capture_output=True, text=True)
        dt = time.time() - t0

        if res.returncode == 0:
            print(f"PASS ({dt:.2f}s)")
            if test_file in pytest_files:
                print(res.stdout, end="")
            passed += 1
        else:
            print(f"FAIL ({dt:.2f}s)")
            print("----------------- STDOUT -----------------")
            print(res.stdout)
            print("----------------- STDERR -----------------")
            print(res.stderr)
            print("------------------------------------------")
            failed += 1

    print(f"==================================================")
    print(f"Summary: {passed} passed, {failed} failed in {time.time() - start_time:.2f}s")
    print(f"==================================================")

    if failed > 0:
        sys.exit(1)


if __name__ == "__main__":
    main()
