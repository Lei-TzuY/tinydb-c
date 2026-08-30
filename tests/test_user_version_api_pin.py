import glob
import os
import subprocess
import sys


def find_binary(repo_root, name):
    candidates = [
        os.path.join(repo_root, "build", "Debug", name + ".exe"),
        os.path.join(repo_root, "build", "Release", name + ".exe"),
        os.path.join(repo_root, "build", name + ".exe"),
        os.path.join(repo_root, "build", name),
    ]
    for path in candidates:
        if os.path.exists(path):
            return path
    raise AssertionError(f"Could not find {name}")


def cleanup(db_path):
    for path in glob.glob(db_path + "*"):
        try:
            os.remove(path)
        except OSError:
            pass


def main():
    repo_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    probe = find_binary(repo_root, "tinydb_user_version_api_pin_probe")
    db_path = os.path.join(os.path.dirname(__file__), "test_user_version_api_pin.db")
    cleanup(db_path)

    try:
        result = subprocess.run(
            [probe, db_path],
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="ignore",
            timeout=120,
        )
        output = result.stdout + "\n" + result.stderr
        assert result.returncode == 0, output
        for marker in (
            "USER_VERSION_API_PIN_OK",
            "busy_nonfatal=yes",
            "zero_on_failure=yes",
            "one_free_frame_success=yes",
            "value_preserved=yes",
            "optional_message=yes",
        ):
            assert marker in result.stdout, output

        print(
            "PASS: linked db_try_get_user_version returns non-fatal backpressure, "
            "zeros output on failure, preserves user_version=77 with one free frame, "
            "and supports callers that omit the diagnostic buffer"
        )
    finally:
        cleanup(db_path)


if __name__ == "__main__":
    try:
        main()
    except AssertionError as exc:
        print("FAIL:", exc)
        sys.exit(1)
