#!/usr/bin/python3

import argparse
import os
import sys
from subprocess import Popen, PIPE, STDOUT, TimeoutExpired
"""
Run the script in the directory where all students' submissions are
"""

# The number of seconds it takes to run the test suite
TIMEOUT = {
    1: 15,
    2: 30,
    3: 30,
}

# ANSI color
ANSI_RED = '\033[31m'
ANSI_GREEN = '\033[32m'
ANSI_RESET = '\033[0m'

# test state
PASSED = 1
FAILED = 0

def check_output(out, test, ofs):
    error = False
    test_passed = False
    out.seek(ofs)
    # read from ofs to EOF
    for line in out:
        if ANSI_GREEN+"passed "+ANSI_RESET+test in line:
            test_passed = True
        if "ERROR" in line or "Assertion failed" in line or "PANIC" in line:
            error = True # can't break here, need to finish reading to EOF
    return test_passed and not error

def test_summary(test_stats, lab):
    score = 0
    if lab == 1:
        for test, result in test_stats.items():
            if result == PASSED:
                score += 10
        assert score <= 100
    if lab == 2:
        for test, result in test_stats.items():
            if result == PASSED:
                score += 10
                if "pipe-robust" in test:
                    score += 10
                elif "race-test" in test:
                    score += 10
        assert score <= 100
    if lab == 3:
        for test, result in test_stats.items():
            if result == PASSED:
                score += 10
                if "cow-fork" in test:
                    score += 20
        assert score <= 100
    print("lab{0}test score: {1}/100".format(lab, score))

def main():
    parser = argparse.ArgumentParser(description="Run osv tests")
    parser.add_argument('lab_number', type=int, help='lab number')
    args = parser.parse_args()

    test_stats = {}
    lab = args.lab_number
    out = open("lab"+str(lab)+"output", "w+")

    # retrieve list of tests for the lab
    testdir = os.path.join(os.getcwd(), "user/lab"+str(lab))
    out.write("test dir: "+testdir+"\n")

    for test in os.listdir(testdir):
        if test.endswith(".c"):
            test = test[:-2]
            out.write("running test: "+test+"\n")
            print("running test: "+test)

        # found test, run in a subprocess
        try:
            ofs = out.tell()
            qemu = Popen(["make", "qemu", "--quiet"], stdin=PIPE, stdout=out, stderr=STDOUT, universal_newlines=True)
            try:
                qemu.communicate(timeout=TIMEOUT[lab], input=test+"\rquit\r")
            except TimeoutExpired as e:
                #print("Exceeded Timeout "+str(TIMEOUT[lab])+ " seconds")
                qemu.terminate()
                pass
            
            # read output from test
            if check_output(out, test, ofs) == True:
                print(ANSI_GREEN + "passed " + ANSI_RESET + test)
                test_stats[test] = PASSED
            else:
                test_stats[test] = FAILED
                print(ANSI_RED + "failed "+ ANSI_RESET + test)
            print('-------------------------------')
        except BrokenPipeError:
            print("fails to start qemu")
            # This just means that QEMU never started
            pass

    # examine test stats
    test_summary(test_stats, lab)
    out.close()

if __name__ == "__main__":
    main()
