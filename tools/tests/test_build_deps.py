import importlib.util
import os
import sys
import unittest

_TOOLS = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, _TOOLS)  # tools/ 便于 deps_lib 导入
_spec = importlib.util.spec_from_file_location(
    "build_deps_mod", os.path.join(_TOOLS, "build-deps.py"))
_mod = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(_mod)
topo_expand = _mod.topo_expand

from deps_lib.manifest import LibSpec


def _lib(name, deps=()):
    return LibSpec(name=name, repo=f"r/{name}", tag="v1", depends_on=deps)


class TestTopoExpand(unittest.TestCase):
    def test_dep_built_before_dependent(self):
        libs = [_lib("B", ("A",)), _lib("A")]
        order = topo_expand(libs, {})
        self.assertEqual([l.name for l in order], ["A", "B"])

    def test_no_duplicate_when_dep_listed(self):
        libs = [_lib("A"), _lib("B", ("A",))]
        order = topo_expand(libs, {})
        self.assertEqual([l.name for l in order], ["A", "B"])

    def test_transitive_deps_expanded(self):
        libs = [_lib("C", ("B",)), _lib("A")]
        gm = {"B": {"repo": "r/B", "tag": "v1", "depends_on": ["A"]}}
        order = topo_expand(libs, gm)
        self.assertEqual([l.name for l in order], ["A", "B", "C"])

    def test_cycle_raises(self):
        libs = [_lib("A", ("B",)), _lib("B", ("A",))]
        with self.assertRaises(RuntimeError):
            topo_expand(libs, {})

    def test_missing_dep_raises(self):
        libs = [_lib("B", ("nope",))]
        with self.assertRaises(RuntimeError):
            topo_expand(libs, {})


if __name__ == "__main__":
    unittest.main()
