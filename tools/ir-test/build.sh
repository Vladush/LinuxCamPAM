#!/bin/bash
# Builds live_scan against the project's existing build/ output, using the same
# compile flags and libraries as linuxcampamd. Doesn't change the project build.
set -e

here=$(cd "$(dirname "$0")" && pwd)
build=$(cd "$here/../.." && pwd)/build

if [ ! -f "$build/libservice_core.a" ]; then
  echo "Build the project first (make all): $build/libservice_core.a is missing." >&2
  exit 1
fi

flags=$build/CMakeFiles/service_core.dir/flags.make
includes=$(sed -n 's/^CXX_INCLUDES = //p' "$flags")
cxx_flags=$(sed -n 's/^CXX_FLAGS = //p' "$flags")
libs=$(sed -E 's/.*(libservice_core\.a)/\1/' "$build/CMakeFiles/linuxcampamd.dir/link.txt")

cd "$build"
# shellcheck disable=SC2086 # These variables hold whole flag lists.
c++ $cxx_flags -DUNIT_TESTING $includes "$here/live_scan.cpp" -o "$here/live_scan" $libs
echo "Built $here/live_scan"
