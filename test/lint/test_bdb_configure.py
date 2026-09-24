# Copyright (c) 2026 The Smartiecoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

"""Exercise the real BDB Autoconf macro in disposable, isolated configure runs.

Run: python3 test/lint/test_bdb_configure.py -v
Requires Autoconf, autoheader, a native C++ compiler and ar. Small compiled API
fixtures make the negative cases deterministic; they are NOT Berkeley DB.
Optional real-library integration cases use BDB_TEST_48_PREFIX and
BDB_TEST_NEW_PREFIX. No daemon, network, wallet or repository build is touched.
Set BDB_CONFIGURE_KEEP_TESTDIR=1 to retain configure logs under TMPDIR.
"""

import os
from pathlib import Path
import shlex
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]


class BDBConfigureTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.env = os.environ.copy()
        for name in ("BDB_CFLAGS", "BDB_LIBS", "CPPFLAGS", "CXXFLAGS", "LDFLAGS",
                     "LIBS", "CPATH", "CPLUS_INCLUDE_PATH", "LIBRARY_PATH",
                     "CONFIG_SITE", "CONFIG_SHELL"):
            cls.env.pop(name, None)
        cls.env.update(CONFIG_SITE=os.devnull, LC_ALL="C")
        cls.cxx = shlex.split(cls.env.get("CXX", "c++"))
        cls.ar = shlex.split(cls.env.get("AR", "ar"))
        cls.autoconf = cls.env.get("AUTOCONF", "autoconf")
        cls.autoheader = cls.env.get("AUTOHEADER", "autoheader")
        for command in (cls.cxx[0], cls.ar[0], cls.autoconf, cls.autoheader):
            if not shutil.which(command):
                raise RuntimeError(f"Required test tool not found: {command}")
        cls.work = Path(tempfile.mkdtemp(prefix="bdb-configure-"))
        if os.environ.get("BDB_CONFIGURE_KEEP_TESTDIR") == "1":
            print(f"Retaining configure evidence: {cls.work}", flush=True)
        else:
            cls.addClassCleanup(shutil.rmtree, cls.work)
        cls.source = cls.work / "source"
        cls.source.mkdir()
        for name in ("bitcoin_find_bdb48.m4", "bitcoin_subdir_to_include.m4"):
            shutil.copyfile(ROOT / "build-aux/m4" / name, cls.source / name)
        # Mirror the options and guarded call in configure.ac; leave the real
        # configure, config headers, Makefiles and all other dependencies alone.
        (cls.source / "configure.ac").write_text("""AC_INIT([bdb-configure-test], [1])
AC_CONFIG_SRCDIR([configure.ac])
AC_CONFIG_HEADERS([config.h])
AC_PROG_CXX
AC_PROG_CXXCPP
AC_LANG([C++])
m4_include([bitcoin_subdir_to_include.m4])
m4_include([bitcoin_find_bdb48.m4])
AC_ARG_ENABLE([wallet], [], [enable_wallet=$enableval], [enable_wallet=auto])
AC_ARG_WITH([bdb], [], [use_bdb=$withval], [use_bdb=auto])
if test "$enable_wallet" != "no"; then
  if test "$use_bdb" != "no"; then
    BITCOIN_FIND_BDB48
  fi
fi
AC_SUBST([use_bdb])
AC_SUBST([CPPFLAGS])
AC_SUBST([LIBS])
AC_CONFIG_FILES([result])
AC_OUTPUT
""", encoding="utf-8")
        (cls.source / "result.in").write_text(
            "use_bdb=@use_bdb@\nBDB_CFLAGS=@BDB_CFLAGS@\n"
            "BDB_CPPFLAGS=@BDB_CPPFLAGS@\nBDB_LIBS=@BDB_LIBS@\n"
            "CPPFLAGS=@CPPFLAGS@\nLIBS=@LIBS@\n", encoding="utf-8")
        cls.command([cls.autoconf], cls.source)
        cls.command([cls.autoheader], cls.source)
        cls.good = cls.fixture("good", (4, 8, 30))
        cls.new = cls.fixture("new", (18, 1, 40))
        cls.old = cls.fixture("old", (4, 7, 25))

    @classmethod
    def command(cls, args, cwd):
        result = subprocess.run(args, cwd=cwd, env=cls.env, text=True,
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                timeout=90, check=False)
        if result.returncode:
            raise RuntimeError(f"{shlex.join(args)} failed:\n{result.stdout}")
        return result.stdout

    @classmethod
    def fixture(cls, name, version):
        prefix = cls.work / name
        (prefix / "include").mkdir(parents=True)
        (prefix / "lib").mkdir()
        major, minor, patch = version
        (prefix / "include/db_cxx.h").write_text(f"""#define DB_VERSION_MAJOR {major}
#define DB_VERSION_MINOR {minor}
#define DB_VERSION_PATCH {patch}
class DbEnv {{
public:
    static char* version(int*, int*, int*);
}};
""", encoding="utf-8")
        (prefix / "api.cpp").write_text(f"""#include <db_cxx.h>
char* DbEnv::version(int* major, int* minor, int* patch) {{
    if (major) *major = {major};
    if (minor) *minor = {minor};
    if (patch) *patch = {patch};
    return nullptr;
}}
""", encoding="utf-8")
        cls.command(cls.cxx + [f"-I{prefix / 'include'}", "-c", "api.cpp", "-o", "api.o"], prefix)
        cls.command(cls.ar + ["crs", "lib/libdb_cxx.a", "api.o"], prefix)
        return prefix

    def configure(self, *options, cflags="", libs="", cppflags="", ldflags=""):
        build = self.work / self.id().rsplit(".", 1)[-1]
        build.mkdir()
        env = self.env.copy()
        env.update(BDB_CFLAGS=cflags, BDB_LIBS=libs, CPPFLAGS=cppflags,
                   LDFLAGS=ldflags, CXXFLAGS="-O0", LIBS="")
        args = [str(self.source / "configure"), "--cache-file=/dev/null", *options]
        result = subprocess.run(args, cwd=build, env=env, text=True,
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                timeout=90, check=False)
        (build / "configure-output.log").write_text(result.stdout, encoding="utf-8")
        self.output = result.stdout
        self.build = build
        return result.returncode

    @staticmethod
    def flags(prefix):
        return {"cflags": f"-I{prefix / 'include'}",
                "libs": str(prefix / "lib/libdb_cxx.a")}

    def assert_enabled(self, returncode):
        self.assertEqual(returncode, 0, self.output)
        self.assertIn("#define USE_BDB 1", (self.build / "config.h").read_text())
        self.assertIn("use_bdb=yes", (self.build / "result").read_text())

    def assert_rejected(self, returncode, diagnostic):
        self.assertNotEqual(returncode, 0, self.output)
        self.assertIn(diagnostic, self.output)
        self.assertFalse((self.build / "config.h").exists(), self.output)

    def test_explicit_48_flags_succeed(self):
        self.assert_enabled(self.configure(**self.flags(self.good)))

    def test_explicit_new_flags_require_opt_in(self):
        self.assert_rejected(self.configure(**self.flags(self.new)), "Berkeley DB 4.8")

    def test_autodetected_new_headers_require_opt_in(self):
        self.assert_rejected(self.configure(
            cppflags=f"-I{self.new / 'include'}", libs=self.flags(self.new)["libs"]),
            "Berkeley DB 4.8")

    def test_without_incompatible_bdb_does_not_opt_in(self):
        self.assert_rejected(self.configure(
            "--without-incompatible-bdb", cppflags=f"-I{self.new / 'include'}",
            libs=self.flags(self.new)["libs"]), "Berkeley DB 4.8")

    def test_explicit_yes_allows_new_headers(self):
        self.assert_enabled(self.configure("--with-incompatible-bdb=yes", **self.flags(self.new)))

    def missing_headers(self):
        missing = self.work / self.id().rsplit(".", 1)[-1].replace("test_", "headers_")
        for version in ("4.8", "48", "4", "5", "5.3", ""):
            for prefix in ("b", "lib", ""):
                path = missing / f"{prefix}db{version}" / "db_cxx.h"
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text('#error "No BDB headers in this fixture"\n', encoding="utf-8")
        (missing / "db_cxx.h").write_text('#error "No BDB headers in this fixture"\n', encoding="utf-8")
        return missing

    def test_missing_required_headers_fail(self):
        missing = self.missing_headers()
        self.assert_rejected(self.configure("--with-bdb", cppflags=f"-I{missing}"),
                             "Berkeley DB C++ headers")

    def test_missing_default_headers_fail_instead_of_disabling_bdb(self):
        self.assert_rejected(self.configure(cppflags=f"-I{self.missing_headers()}"),
                             "Berkeley DB C++ headers")

    def test_incompatible_opt_in_still_requires_supported_headers(self):
        self.assert_rejected(self.configure("--with-incompatible-bdb=yes", **self.flags(self.old)),
                             "Berkeley DB C++ headers")

    def test_missing_explicit_headers_with_opt_in_fail(self):
        self.assert_rejected(self.configure(
            "--with-incompatible-bdb=yes", cflags=f"-I{self.missing_headers()}",
            libs=self.flags(self.good)["libs"]), "Berkeley DB C++ headers")

    def empty_libraries(self):
        prefix = self.work / self.id().rsplit(".", 1)[-1].replace("test_", "libs_")
        prefix.mkdir()
        (prefix / "empty.cpp").write_text("int not_a_bdb_api() { return 0; }\n", encoding="utf-8")
        self.command(self.cxx + ["-c", "empty.cpp", "-o", "empty.o"], prefix)
        for library in ("db_cxx-4.8", "db_cxx", "db4_cxx"):
            self.command(self.ar + ["crs", f"lib{library}.a", "empty.o"], prefix)
        return prefix

    def test_missing_explicit_library_fails(self):
        self.assert_rejected(self.configure(
            cflags=self.flags(self.good)["cflags"],
            libs=str(self.work / "does-not-exist.a")), "Berkeley DB C++ library")

    def test_explicit_library_without_bdb_api_fails(self):
        libraries = self.empty_libraries()
        self.assert_rejected(self.configure(
            cflags=self.flags(self.good)["cflags"],
            libs=str(libraries / "libdb_cxx.a")), "Berkeley DB C++ library")

    def test_autodetected_library_without_bdb_api_fails(self):
        libraries = self.empty_libraries()
        self.assert_rejected(self.configure(
            cflags=self.flags(self.good)["cflags"], ldflags=f"-L{libraries}"),
            "Berkeley DB C++ library")

    def test_autodetected_library_links_real_api(self):
        self.assert_enabled(self.configure(cflags=self.flags(self.good)["cflags"],
                                           ldflags=f"-L{self.good / 'lib'}"))

    def test_native_header_library_version_mismatch_fails(self):
        self.assert_rejected(self.configure(
            cflags=self.flags(self.good)["cflags"], libs=self.flags(self.new)["libs"]),
            "Berkeley DB header/library version mismatch")

    def test_incompatible_opt_in_does_not_allow_header_library_mismatch(self):
        self.assert_rejected(self.configure(
            "--with-incompatible-bdb=yes", cflags=self.flags(self.good)["cflags"],
            libs=self.flags(self.new)["libs"]), "Berkeley DB header/library version mismatch")

    def test_cross_compile_skips_runtime_agreement_but_links(self):
        self.assert_enabled(self.configure(
            "--build=bdb-test-build", "--host=bdb-test-host",
            cflags=self.flags(self.good)["cflags"], libs=self.flags(self.new)["libs"]))
        self.assertIn("checking whether we are cross compiling... yes", self.output)
        self.assertIn("runtime header/library version agreement not checked", self.output)

    def test_cross_compile_still_requires_linkable_api(self):
        libraries = self.empty_libraries()
        self.assert_rejected(self.configure(
            "--build=bdb-test-build", "--host=bdb-test-host",
            cflags=self.flags(self.good)["cflags"], libs=str(libraries / "libdb_cxx.a")),
            "Berkeley DB C++ library")


    def test_without_bdb_preserves_intentional_disabled_path(self):
        self.assertEqual(self.configure("--without-bdb", cflags="-I/missing", libs="/missing.a"),
                         0, self.output)
        self.assertNotIn("#define USE_BDB 1", (self.build / "config.h").read_text())
        self.assertNotIn("checking for Berkeley DB", self.output)

    def test_disable_wallet_preserves_intentional_disabled_path(self):
        self.assertEqual(self.configure("--disable-wallet", "--with-bdb",
                                        cflags="-I/missing", libs="/missing.a"), 0, self.output)
        self.assertNotIn("#define USE_BDB 1", (self.build / "config.h").read_text())
        self.assertNotIn("checking for Berkeley DB", self.output)

    def test_bare_incompatible_option_preserves_local_builds(self):
        self.assert_enabled(self.configure("--with-incompatible-bdb", **self.flags(self.new)))
        self.assertIn("wallets opened by this build will not be portable", self.output)

    def test_invalid_incompatible_option_is_rejected(self):
        self.assert_rejected(self.configure("--with-incompatible-bdb=maybe", **self.flags(self.good)),
                             "accepts only yes or no")

    def test_manual_flags_keep_precedence_and_do_not_leak(self):
        cppflags = f"-DKEEP_CPPFLAGS=1 -I{self.new / 'include'}"
        self.assert_enabled(self.configure(cppflags=cppflags, **self.flags(self.good)))
        result = dict(line.split("=", 1) for line in (self.build / "result").read_text().splitlines())
        self.assertEqual(result["CPPFLAGS"], cppflags)
        self.assertEqual(result["LIBS"], "")
        self.assertEqual(result["BDB_CFLAGS"], self.flags(self.good)["cflags"])
        self.assertEqual(result["BDB_CPPFLAGS"], "")

    def real_flags(self, variable):
        if variable not in os.environ:
            self.skipTest(f"Set {variable} to exercise a real installed Berkeley DB")
        prefix = Path(os.environ[variable]).resolve()
        self.assertTrue((prefix / "include/db_cxx.h").is_file(), str(prefix))
        self.assertTrue((prefix / "include/db.h").is_file(), str(prefix))
        return {"cflags": f"-I{prefix / 'include'}",
                "libs": f"-L{prefix / 'lib'} -ldb_cxx"}

    def test_real_48_explicit_flags_succeed(self):
        self.assert_enabled(self.configure("--with-bdb", **self.real_flags("BDB_TEST_48_PREFIX")))
        self.assertIn("headers and library versions agree... yes", self.output)

    def test_real_new_explicit_flags_rejected_by_default(self):
        self.assert_rejected(self.configure(**self.real_flags("BDB_TEST_NEW_PREFIX")), "Berkeley DB 4.8")

    def test_real_new_explicit_flags_rejected_with_without_option(self):
        self.assert_rejected(self.configure("--without-incompatible-bdb",
                                            **self.real_flags("BDB_TEST_NEW_PREFIX")), "Berkeley DB 4.8")

    def test_real_new_explicit_yes_local_build_succeeds(self):
        self.assert_enabled(self.configure("--with-incompatible-bdb=yes",
                                           **self.real_flags("BDB_TEST_NEW_PREFIX")))
        self.assertIn("headers and library versions agree... yes", self.output)

    def test_real_native_header_library_version_mismatch_fails(self):
        good = self.real_flags("BDB_TEST_48_PREFIX")
        new = self.real_flags("BDB_TEST_NEW_PREFIX")
        self.assert_rejected(self.configure(cflags=good["cflags"], libs=new["libs"]),
                             "Berkeley DB header/library version mismatch")


if __name__ == "__main__":
    unittest.main()
