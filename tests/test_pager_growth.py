import glob
import os
import shutil
import subprocess
import tempfile


def find_probe(repo_root):
    candidates = [
        os.path.join(repo_root, 'build', 'tinydb_pager_growth_probe'),
        os.path.join(repo_root, 'build', 'Debug', 'tinydb_pager_growth_probe.exe'),
        os.path.join(repo_root, 'build', 'Release', 'tinydb_pager_growth_probe.exe'),
    ]
    return next((path for path in candidates if os.path.exists(path)), None)


def main():
    repo_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    probe = find_probe(repo_root)
    if probe is None:
        raise AssertionError('tinydb_pager_growth_probe executable was not built')

    temp_dir = tempfile.mkdtemp(prefix='tinydb-pager-growth-')
    db_path = os.path.join(temp_dir, 'growth.db')
    try:
        result = subprocess.run(
            [probe, db_path],
            capture_output=True,
            text=True,
            timeout=180,
        )
        assert result.returncode == 0, result.stdout + '\n' + result.stderr
        assert 'PAGER_GROWTH_OK' in result.stdout, result.stdout
        assert 'legacy_ceiling=4096' in result.stdout, result.stdout
        assert 'pager_publish_pages=64' in result.stdout, result.stdout
        assert 'buffer_pool=16' in result.stdout, result.stdout
        assert 'eviction_safe=yes' in result.stdout, result.stdout
        assert 'publish_rollback=yes' in result.stdout, result.stdout

        # The probe writes pages 0..4128 inclusive, proving that the old
        # TABLE_MAX_PAGES boundary is no longer a Pager allocation limit. It
        # then publishes and rolls back 64 staged page images through a 16-frame
        # LRU pool, proving recursive staged publication need not retain every
        # target frame simultaneously.
        expected_pages = 4096 + 32 + 1
        assert os.path.getsize(db_path) == expected_pages * 4096
    finally:
        for path in glob.glob(db_path + '*'):
            try:
                os.remove(path)
            except OSError:
                pass
        shutil.rmtree(temp_dir, ignore_errors=True)


if __name__ == '__main__':
    main()
