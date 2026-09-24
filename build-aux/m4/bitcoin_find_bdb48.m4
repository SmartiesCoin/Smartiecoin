dnl Copyright (c) 2013-2015 The Bitcoin Core developers
dnl Distributed under the MIT software license, see the accompanying
dnl file COPYING or http://www.opensource.org/licenses/mit-license.php.

AC_DEFUN([BITCOIN_FIND_BDB48],[
  AC_ARG_VAR([BDB_CFLAGS], [Compiler flags for Berkeley DB (still validated)])
  AC_ARG_VAR([BDB_LIBS], [Linker flags for Berkeley DB (still validated)])
  AC_ARG_WITH([incompatible-bdb],
    [AS_HELP_STRING([--with-incompatible-bdb], [allow Berkeley DB other than 4.8 (non-portable wallets, local testing only)])],
    [with_incompatible_bdb=$withval], [with_incompatible_bdb=no])
  AS_CASE([$with_incompatible_bdb], [yes|no], [],
    [AC_MSG_ERROR([--with-incompatible-bdb accepts only yes or no])])

  if test "$use_bdb" = "no"; then
    use_bdb=no
  elif test "$BDB_CFLAGS" = ""; then
    AC_MSG_CHECKING([for Berkeley DB C++ headers])
    BDB_CPPFLAGS=
    bdbpath=X
    bdb48path=X
    bdbdirlist=
    for _vn in 4.8 48 4 5 5.3 ''; do
      for _pfx in b lib ''; do
        bdbdirlist="$bdbdirlist ${_pfx}db${_vn}"
      done
    done
    for searchpath in $bdbdirlist ''; do
      test -n "${searchpath}" && searchpath="${searchpath}/"
      AC_COMPILE_IFELSE([AC_LANG_PROGRAM([[
        #include <${searchpath}db_cxx.h>
      ]],[[
        #if !((DB_VERSION_MAJOR == 4 && DB_VERSION_MINOR >= 8) || DB_VERSION_MAJOR > 4)
          #error "failed to find bdb 4.8+"
        #endif
      ]])],[
        if test "$bdbpath" = "X"; then
          bdbpath="${searchpath}"
        fi
      ],[
        continue
      ])
      AC_COMPILE_IFELSE([AC_LANG_PROGRAM([[
        #include <${searchpath}db_cxx.h>
      ]],[[
        #if !(DB_VERSION_MAJOR == 4 && DB_VERSION_MINOR == 8)
          #error "failed to find bdb 4.8"
        #endif
      ]])],[
        bdb48path="${searchpath}"
        break
      ],[])
    done
    if test "$bdbpath" = "X"; then
      AC_MSG_RESULT([no])
      AC_MSG_ERROR([Berkeley DB C++ headers missing or unsupported. Install Berkeley DB 4.8, or use --without-bdb to disable BDB wallet support.])
    elif test "$bdb48path" = "X"; then
      BITCOIN_SUBDIR_TO_INCLUDE(BDB_CPPFLAGS,[${bdbpath}],db_cxx)
      use_bdb=yes
    else
      BITCOIN_SUBDIR_TO_INCLUDE(BDB_CPPFLAGS,[${bdb48path}],db_cxx)
      bdbpath="${bdb48path}"
      use_bdb=yes
    fi
  else
    dnl BDB_CFLAGS is used directly by the wallet and Qt build targets. Do not
    dnl duplicate it into BDB_CPPFLAGS, because warning suppression can convert
    dnl it to -isystem and make Clang prefer a later -I from depends.
    BDB_CPPFLAGS=
  fi
  AC_SUBST(BDB_CPPFLAGS)

  if test "$use_bdb" != "no"; then
    bdb_save_CPPFLAGS="$CPPFLAGS"
    bdb_save_LIBS="$LIBS"
    CPPFLAGS="$BDB_CFLAGS $BDB_CPPFLAGS $CPPFLAGS"
    AC_MSG_CHECKING([for usable Berkeley DB C++ headers])
    AC_COMPILE_IFELSE([AC_LANG_PROGRAM([[
      #include <db_cxx.h>
      #if !((DB_VERSION_MAJOR == 4 && DB_VERSION_MINOR >= 8) || DB_VERSION_MAJOR > 4)
        #error "Berkeley DB 4.8 or newer required"
      #endif
    ]], [[]])], [AC_MSG_RESULT([yes])], [
      AC_MSG_RESULT([no])
      AC_MSG_ERROR([Berkeley DB C++ headers missing or unsupported. Check BDB_CFLAGS, or use --without-bdb to disable BDB wallet support.])
    ])
    AC_MSG_CHECKING([for Berkeley DB 4.8 headers])
    AC_COMPILE_IFELSE([AC_LANG_PROGRAM([[
      #include <db_cxx.h>
      #if DB_VERSION_MAJOR != 4 || DB_VERSION_MINOR != 8
        #error "Berkeley DB 4.8 required"
      #endif
    ]], [[]])], [AC_MSG_RESULT([yes])], [
      AC_MSG_RESULT([no])
      if test "$with_incompatible_bdb" != "yes"; then
        AC_MSG_ERROR([Berkeley DB 4.8 is required for portable BDB wallets. Use --with-incompatible-bdb=yes only for local testing, or --without-bdb to disable BDB wallet support.])
      fi
      AC_MSG_WARN([Found Berkeley DB other than 4.8; BDB (legacy) wallets opened by this build will not be portable!])
    ])
    if test "$BDB_LIBS" = ""; then
      for searchlib in db_cxx-4.8 db_cxx db4_cxx; do
        AC_MSG_CHECKING([for Berkeley DB C++ API in -l$searchlib])
        LIBS="-l$searchlib $bdb_save_LIBS"
        AC_LINK_IFELSE([AC_LANG_PROGRAM([[#include <db_cxx.h>]], [[
          int major, minor, patch;
          DbEnv::version(&major, &minor, &patch);
        ]])], [
          AC_MSG_RESULT([yes])
          BDB_LIBS="-l$searchlib"
          break
        ], [AC_MSG_RESULT([no])])
      done
    fi
    if test "$BDB_LIBS" = ""; then
      AC_MSG_ERROR([Berkeley DB C++ library missing or unusable. Install Berkeley DB 4.8, set BDB_LIBS, or use --without-bdb to disable BDB wallet support.])
    fi
    LIBS="$BDB_LIBS $bdb_save_LIBS"
    AC_MSG_CHECKING([whether the Berkeley DB C++ library links with the selected headers])
    AC_LINK_IFELSE([AC_LANG_PROGRAM([[#include <db_cxx.h>]], [[
      int major, minor, patch;
      DbEnv::version(&major, &minor, &patch);
    ]])], [AC_MSG_RESULT([yes])], [
      AC_MSG_RESULT([no])
      AC_MSG_ERROR([Berkeley DB C++ library cannot link with the selected headers. Check BDB_CFLAGS and BDB_LIBS, or use --without-bdb to disable BDB wallet support.])
    ])
    AC_MSG_CHECKING([whether Berkeley DB headers and library versions agree])
    AC_RUN_IFELSE([AC_LANG_PROGRAM([[#include <db_cxx.h>]], [[
      int major, minor, patch;
      DbEnv::version(&major, &minor, &patch);
      return major != DB_VERSION_MAJOR || minor != DB_VERSION_MINOR || patch != DB_VERSION_PATCH;
    ]])], [AC_MSG_RESULT([yes])], [
      AC_MSG_RESULT([no])
      AC_MSG_ERROR([Berkeley DB header/library version mismatch, or test executable could not run. Check BDB_CFLAGS, BDB_LIBS and the runtime library search path; see config.log.])
    ], [
      AC_MSG_RESULT([not checked (cross compiling)])
      AC_MSG_WARN([Berkeley DB runtime header/library version agreement not checked when cross compiling; only API linkability was tested.])
    ])
    CPPFLAGS="$bdb_save_CPPFLAGS"
    LIBS="$bdb_save_LIBS"
    AC_DEFINE([USE_BDB], [1], [Define if BDB support should be compiled in])
    use_bdb=yes
  fi
])
