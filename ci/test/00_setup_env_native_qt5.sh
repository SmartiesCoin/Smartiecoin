#!/usr/bin/env bash
#
# Copyright (c) 2019-2021 The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

export LC_ALL=C.UTF-8

export CONTAINER_NAME=ci_native_qt5
export HOST=x86_64-pc-linux-gnu
export PACKAGES="python3-zmq qtbase5-dev qttools5-dev-tools libdbus-1-dev libharfbuzz-dev"
export DEP_OPTS=""
export TEST_RUNNER_EXTRA="--coverage --extended --exclude feature_pruning,feature_dbcrash"  # Run extended tests so that coverage does not fail, but exclude the very slow dbcrash. --previous-releases removed: SMT has no downloadable Dash-style release ladder (see DOWNLOAD_PREVIOUS_RELEASES below); the four *_compatibility tests self-skip.
export RUN_UNIT_TESTS_SEQUENTIAL="true"
export RUN_UNIT_TESTS="false"
export GOAL="install"
export DOWNLOAD_PREVIOUS_RELEASES="false"  # Smartiecoin has no Dash-style release ladder: get_previous_releases.py points at dashpay/smartiecoin (nonexistent) and the compat tests reference Dash versions (v21.1.1, v19.3.0, v0.12.1.5) that SMT never had. Disabled so the four *_compatibility tests self-skip (skip_if_no_previous_releases) instead of failing the whole job on a broken download. Wallet backward-compat for SMT is covered by the v0.5.1->0.5.3 package QA harness.
export BITCOIN_CONFIG="--enable-zmq --with-libs=no --enable-reduce-exports LDFLAGS=-static-libstdc++"
