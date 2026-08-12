import subprocess
import os
import sys

def run_test():
    base_dir = os.path.join(os.path.dirname(__file__), '..')
    possible_paths = [
        os.path.join(base_dir, 'build', 'Debug', 'tinydb.exe'),
        os.path.join(base_dir, 'build', 'Release', 'tinydb.exe'),
        os.path.join(base_dir, 'build', 'tinydb.exe'),
        os.path.join(base_dir, 'build', 'tinydb')
    ]
    
    executable_path = None
    for path in possible_paths:
        if os.path.exists(path):
            executable_path = path
            break
            
    if not executable_path:
        print("Could not find the tinydb executable.")
        sys.exit(1)
        
    db_file = os.path.join(os.path.dirname(__file__), 'test_join.db')
    if os.path.exists(db_file):
        try:
            os.remove(db_file)
        except Exception:
            pass

    commands = (
        "insert 1 alice alice@test.com\n"
        "insert 2 bob bob@test.com\n"
        "select * from users join posts on users.id = posts.user_id;\n"
        ".exit\n"
    )
    
    process = subprocess.Popen(
        [executable_path, db_file],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        encoding='utf-8',
        errors='ignore'
    )
    
    stdout, stderr = process.communicate(input=commands)
    
    if process.returncode != 0:
        print(f"FAIL: JOIN test failed with return code {process.returncode}")
        print("STDOUT:", stdout)
        print("STDERR:", stderr)
        sys.exit(1)

    if "(1, alice, alice@test.com) | (1, alice, alice@test.com)" not in stdout:
        print("FAIL: Expected JOIN output for matching records.")
        print("STDOUT:", stdout)
        sys.exit(1)

    print("PASS: Cross-Table INNER JOIN Query verified successfully!")

    if os.path.exists(db_file):
        try:
            os.remove(db_file)
        except Exception:
            pass

if __name__ == "__main__":
    run_test()
