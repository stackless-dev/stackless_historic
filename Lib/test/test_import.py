from test.test_support import TESTFN, run_unittest, catch_warning

import unittest
import os
import random
import shutil
import sys
import py_compile
import warnings
from test.test_support import unlink, TESTFN, unload


def remove_files(name):
    for f in (name + os.extsep + "py",
              name + os.extsep + "pyc",
              name + os.extsep + "pyo",
              name + os.extsep + "pyw",
              name + "$py.class"):
        if os.path.exists(f):
            os.remove(f)


class ImportTest(unittest.TestCase):

    def testCaseSensitivity(self):
        # Brief digression to test that import is case-sensitive:  if we got this
        # far, we know for sure that "random" exists.
        try:
            import RAnDoM
        except ImportError:
            pass
        else:
            self.fail("import of RAnDoM should have failed (case mismatch)")

    def testDoubleConst(self):
        # Another brief digression to test the accuracy of manifest float constants.
        from test import double_const  # don't blink -- that *was* the test

    def testImport(self):
        def test_with_extension(ext):
            # ext normally ".py"; perhaps ".pyw"
            source = TESTFN + ext
            pyo = TESTFN + os.extsep + "pyo"
            if sys.platform.startswith('java'):
                pyc = TESTFN + "$py.class"
            else:
                pyc = TESTFN + os.extsep + "pyc"

            f = open(source, "w")
            print >> f, "# This tests Python's ability to import a", ext, "file."
            a = random.randrange(1000)
            b = random.randrange(1000)
            print >> f, "a =", a
            print >> f, "b =", b
            f.close()

            try:
                try:
                    mod = __import__(TESTFN)
                except ImportError, err:
                    self.fail("import from %s failed: %s" % (ext, err))

                self.assertEquals(mod.a, a,
                    "module loaded (%s) but contents invalid" % mod)
                self.assertEquals(mod.b, b,
                    "module loaded (%s) but contents invalid" % mod)
            finally:
                os.unlink(source)

            try:
                try:
                    reload(mod)
                except ImportError, err:
                    self.fail("import from .pyc/.pyo failed: %s" % err)
            finally:
                try:
                    os.unlink(pyc)
                except OSError:
                    pass
                try:
                    os.unlink(pyo)
                except OSError:
                    pass
                del sys.modules[TESTFN]

        sys.path.insert(0, os.curdir)
        try:
            test_with_extension(os.extsep + "py")
            if sys.platform.startswith("win"):
                for ext in ".PY", ".Py", ".pY", ".pyw", ".PYW", ".pYw":
                    test_with_extension(ext)
        finally:
            del sys.path[0]

    def testImpModule(self):
        # Verify that the imp module can correctly load and find .py files
        import imp
        x = imp.find_module("os")
        os = imp.load_module("os", *x)

    def test_module_with_large_stack(self, module='longlist'):
        # create module w/list of 65000 elements to test bug #561858
        filename = module + os.extsep + 'py'

        # create a file with a list of 65000 elements
        f = open(filename, 'w+')
        f.write('d = [\n')
        for i in range(65000):
            f.write('"",\n')
        f.write(']')
        f.close()

        # compile & remove .py file, we only need .pyc (or .pyo)
        f = open(filename, 'r')
        py_compile.compile(filename)
        f.close()
        os.unlink(filename)

        # need to be able to load from current dir
        sys.path.append('')

        # this used to crash
        exec 'import ' + module

        # cleanup
        del sys.path[-1]
        for ext in 'pyc', 'pyo':
            fname = module + os.extsep + ext
            if os.path.exists(fname):
                os.unlink(fname)

    def test_failing_import_sticks(self):
        source = TESTFN + os.extsep + "py"
        f = open(source, "w")
        print >> f, "a = 1/0"
        f.close()

        # New in 2.4, we shouldn't be able to import that no matter how often
        # we try.
        sys.path.insert(0, os.curdir)
        try:
            for i in 1, 2, 3:
                try:
                    mod = __import__(TESTFN)
                except ZeroDivisionError:
                    if TESTFN in sys.modules:
                        self.fail("damaged module in sys.modules on %i. try" % i)
                else:
                    self.fail("was able to import a damaged module on %i. try" % i)
        finally:
            sys.path.pop(0)
            remove_files(TESTFN)

    def test_failing_reload(self):
        # A failing reload should leave the module object in sys.modules.
        source = TESTFN + os.extsep + "py"
        f = open(source, "w")
        print >> f, "a = 1"
        print >> f, "b = 2"
        f.close()

        sys.path.insert(0, os.curdir)
        try:
            mod = __import__(TESTFN)
            self.assert_(TESTFN in sys.modules, "expected module in sys.modules")
            self.assertEquals(mod.a, 1, "module has wrong attribute values")
            self.assertEquals(mod.b, 2, "module has wrong attribute values")

            # On WinXP, just replacing the .py file wasn't enough to
            # convince reload() to reparse it.  Maybe the timestamp didn't
            # move enough.  We force it to get reparsed by removing the
            # compiled file too.
            remove_files(TESTFN)

            # Now damage the module.
            f = open(source, "w")
            print >> f, "a = 10"
            print >> f, "b = 20//0"
            f.close()

            self.assertRaises(ZeroDivisionError, reload, mod)

            # But we still expect the module to be in sys.modules.
            mod = sys.modules.get(TESTFN)
            self.failIf(mod is None, "expected module to still be in sys.modules")

            # We should have replaced a w/ 10, but the old b value should
            # stick.
            self.assertEquals(mod.a, 10, "module has wrong attribute values")
            self.assertEquals(mod.b, 2, "module has wrong attribute values")

        finally:
            sys.path.pop(0)
            remove_files(TESTFN)
            if TESTFN in sys.modules:
                del sys.modules[TESTFN]

    def test_infinite_reload(self):
        # Bug #742342 reports that Python segfaults (infinite recursion in C)
        #  when faced with self-recursive reload()ing.

        sys.path.insert(0, os.path.dirname(__file__))
        try:
            import infinite_reload
        finally:
            sys.path.pop(0)

    def test_import_name_binding(self):
        # import x.y.z binds x in the current namespace
        import test as x
        import test.test_support
        self.assert_(x is test, x.__name__)
        self.assert_(hasattr(test.test_support, "__file__"))

        # import x.y.z as w binds z as w
        import test.test_support as y
        self.assert_(y is test.test_support, y.__name__)

    def test_import_initless_directory_warning(self):
        with catch_warning():
            # Just a random non-package directory we always expect to be
            # somewhere in sys.path...
            warnings.simplefilter('error', ImportWarning)
            self.assertRaises(ImportWarning, __import__, "site-packages")

    def test_importbyfilename(self):
        path = os.path.abspath(TESTFN)
        try:
            __import__(path)
        except ImportError, err:
            self.assertEqual("Import by filename is not supported.",
                              err.args[0])
        else:
            self.fail("import by path didn't raise an exception")

class PathsTests(unittest.TestCase):
    path = TESTFN

    def setUp(self):
        os.mkdir(self.path)
        self.syspath = sys.path[:]

    def tearDown(self):
        shutil.rmtree(self.path)
        sys.path = self.syspath

    # http://bugs.python.org/issue1293
    def test_trailing_slash(self):
        f = open(os.path.join(self.path, 'test_trailing_slash.py'), 'w')
        f.write("testdata = 'test_trailing_slash'")
        f.close()
        sys.path.append(self.path+'/')
        mod = __import__("test_trailing_slash")
        self.assertEqual(mod.testdata, 'test_trailing_slash')
        unload("test_trailing_slash")

class RelativeImport(unittest.TestCase):
    def tearDown(self):
        try:
            del sys.modules["test.relimport"]
        except:
            pass

    def test_relimport_star(self):
        # This will import * from .test_import.
        from . import relimport
        self.assertTrue(hasattr(relimport, "RelativeImport"))

def test_main(verbose=None):
    run_unittest(ImportTest, PathsTests, RelativeImport)

if __name__ == '__main__':
    # test needs to be a package, so we can do relative import
    from test.test_import import test_main
    test_main()
