import glob
import os
import shutil
import subprocess
import tempfile


def benchmark_executable(repo_root):
    candidates = [
        os.path.join(repo_root, "build", "tinydb_bench"),
        os.path.join(repo_root, "build", "Debug", "tinydb_bench.exe"),
        os.path.join(repo_root, "build", "Release", "tinydb_bench.exe"),
    ]
    for path in candidates:
        if os.path.exists(path):
            return path
    raise AssertionError("tinydb_bench executable was not built")


def main():
    repo_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    exe = benchmark_executable(repo_root)
    temp_dir = tempfile.mkdtemp(prefix="tinydb-bench-test-")
    db_path = os.path.join(temp_dir, "smoke.db")

    try:
        result = subprocess.run(
            [exe, db_path, "200", "500"],
            capture_output=True,
            text=True,
            timeout=60,
        )
        assert result.returncode == 0, result.stdout + "\n" + result.stderr
        assert "inserted=200" in result.stdout, result.stdout
        assert "lookup_hits=500" in result.stdout, result.stdout
        assert "rows=200" in result.stdout, result.stdout
        assert "BENCHMARK_OK" in result.stdout, result.stdout

        # The benchmark intentionally refuses to overwrite an existing DB.
        second = subprocess.run(
            [exe, db_path, "10", "10"],
            capture_output=True,
            text=True,
            timeout=30,
        )
        assert second.returncode == 2, second.stdout + "\n" + second.stderr
        assert "already exists" in second.stderr, second.stderr
    finally:
        for path in glob.glob(db_path + "*"):
            try:
                os.remove(path)
            except OSError:
                pass
        shutil.rmtree(temp_dir, ignore_errors=True)


if __name__ == "__main__":
    main()
