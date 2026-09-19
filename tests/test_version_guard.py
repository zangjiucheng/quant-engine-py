"""PER-239 — the import-time assert that this package exists for.

The failure being guarded is not a crash. It is a binding compiled
against one engine and read as though it spoke for another: it imports,
it runs, and it returns *subtly different numbers*. Nothing downstream
can detect that, which is why the check has to raise at import rather
than warn.
"""

import unittest

import qe


class VersionGuard(unittest.TestCase):
    def test_the_binding_reports_what_it_was_built_against(self):
        self.assertTrue(qe.engine_version, "engine_version is empty")
        self.assertRegex(qe.engine_version, r"^\d+\.\d+\.\d+$")
        self.assertRegex(qe.engine_commit, r"^[0-9a-f]{40}$")

    def test_the_version_comes_from_the_engines_own_header(self):
        # `qe::KVersion`, generated from the engine's PROJECT_VERSION.
        # Not a CMake define: `project()`'s `<name>_VERSION` is
        # directory-scoped and came through EMPTY when this was first
        # wired, which is how that was caught.
        self.assertIn(qe.engine_version, qe.engine_describe)

    def test_a_moved_submodule_raises(self):
        """The guard, driven RED.

        Substitutes what the working tree reports, not what the
        extension was compiled with — so this exercises the comparison
        against a genuinely different commit rather than asserting that
        a rewritten constant differs from itself.
        """
        real = qe._submodule_head
        try:
            qe._submodule_head = lambda: "0" * 40
            with self.assertRaises(qe.StaleBindingError) as ctx:
                qe._check()
        finally:
            qe._submodule_head = real
        msg = str(ctx.exception)
        self.assertIn(qe.engine_commit[:12], msg)
        self.assertIn("000000000000", msg)
        # The message has to say what to DO. A stale binding is found by
        # someone who does not yet know why their numbers moved.
        self.assertIn("pip install", msg)

    def test_no_source_tree_is_not_a_failure(self):
        # A wheel installed away from its submodule has nothing to
        # compare against. That is a different situation from a
        # mismatch, and inventing a check there would make every
        # installed wheel unimportable.
        real = qe._submodule_head
        try:
            qe._submodule_head = lambda: None
            qe._check()  # must not raise
        finally:
            qe._submodule_head = real


if __name__ == "__main__":
    unittest.main(verbosity=2)
