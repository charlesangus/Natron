#!/usr/bin/env python3
"""Run the vendored tabtabtab core tests and the Natron adapter tests with
no pytest dependency, so ctest's TabTabTabPython case needs only the host's
python3.
"""

import importlib.util
import os
import sys
import traceback
import unittest


_THIS_DIR = os.path.dirname(os.path.abspath(__file__))
_REPO_ROOT = os.path.dirname(os.path.dirname(_THIS_DIR))
_TABTABTAB_DIR = os.path.join(_REPO_ROOT, "Gui", "Resources", "TabTabTab")
_VENDORED_TESTS_DIR = os.path.join(_TABTABTAB_DIR, "tests")


def _run_vendored_tests(failures):
    for name in sorted(os.listdir(_VENDORED_TESTS_DIR)):
        if not name.startswith("test_") or not name.endswith(".py"):
            continue
        path = os.path.join(_VENDORED_TESTS_DIR, name)
        spec = importlib.util.spec_from_file_location(name[:-3], path)
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)

        for attr_name in sorted(vars(module)):
            if not attr_name.startswith("test_"):
                continue
            func = getattr(module, attr_name)
            if not callable(func):
                continue
            try:
                func()
            except Exception:
                failures.append("%s:%s\n%s" % (name, attr_name, traceback.format_exc()))


def _run_adapter_tests(failures):
    loader = unittest.TestLoader()
    suite = loader.discover(start_dir=_THIS_DIR, top_level_dir=_THIS_DIR)
    result = unittest.TestResult()
    suite.run(result)

    for test, err in result.errors + result.failures:
        failures.append("%s\n%s" % (test, err))


def main():
    sys.path.insert(0, _TABTABTAB_DIR)
    sys.path.insert(0, _THIS_DIR)

    failures = []
    _run_vendored_tests(failures)
    _run_adapter_tests(failures)

    for failure in failures:
        print(failure)

    if failures:
        print("%d failure(s)" % len(failures))
        return 1

    print("All tests passed.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
