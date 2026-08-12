import glob
import os
import subprocess
import sys
import time

def main():
    base_dir = os.path.dirname(os.path.abspath(__file__))
    test_files = sorted(glob.glob(os.path.join(base_dir, "test_*.py")))
    
    print(f"==================================================")
    print(f"Running {len(test_files)} Tiny Database test suites...")
    print(f"==================================================")
    
    passed = 0
    failed = 0
    start_time = time.time()
    
    for test_file in test_files:
        name = os.path.basename(test_file)
        print(f"Running {name:<32} ... ", end="", flush=True)
        t0 = time.time()
        res = subprocess.run([sys.executable, test_file], capture_output=True, text=True)
        dt = time.time() - t0
        
        if res.returncode == 0:
            print(f"PASS ({dt:.2f}s)")
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
