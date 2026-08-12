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
        
    db_file = os.path.join(os.path.dirname(__file__), 'test_multi_tbl.db')
    if os.path.exists(db_file):
        try:
            os.remove(db_file)
        except Exception:
            pass

    commands = (
        "create table posts (id INT, title VARCHAR, user_id INT);\n"
        "create table orders (id INT, item VARCHAR, price INT);\n"
        ".tables\n"
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
        print(f"FAIL: Multi-table test failed with return code {process.returncode}")
        print("STDOUT:", stdout)
        print("STDERR:", stderr)
        sys.exit(1)

    if "users" not in stdout or "posts" not in stdout or "orders" not in stdout:
        print("FAIL: Expected tables users, posts, orders in .tables output.")
        print("STDOUT:", stdout)
        sys.exit(1)

    print("PASS: Dynamic Multi-Table Schema & System Catalog verified successfully!")

    if os.path.exists(db_file):
        try:
            os.remove(db_file)
        except Exception:
            pass

if __name__ == "__main__":
    run_test()
