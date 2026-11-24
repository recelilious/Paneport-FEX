#!/bin/sh
# SPDX-License-Identifier: MIT
# SPDX-FileCopyrightText: 2026 recelilious and the Paneport contributors

# Builds SharedUserDataTest.cpp with the decoder of Source/Windows/Common/SharedUserData.cpp, under AddressSanitizer and
# UndefinedBehaviorSanitizer, and runs it, on 64-bit Linux or macOS. CXX names the compiler, clang++ by default. The program
# is built in a temporary directory, which is removed afterwards.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../../.." && pwd)
out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT
"${CXX:-clang++}" -std=c++20 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all \
  -I "$here/Stub" -I "$root/Source" -o "$out/SharedUserDataTest" \
  "$here/SharedUserDataTest.cpp" "$root/Source/Windows/Common/SharedUserData.cpp"
"$out/SharedUserDataTest"
