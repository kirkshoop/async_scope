#!/bin/bash

realpath() {
    [[ $1 = /* ]] && echo "$1" || echo "$PWD/${1#./}"
}
CURDIR=$(realpath $(dirname "$0"))
source ${CURDIR}/_test_common.sh

startTest "clang-12 compilation"

# Create a temporary directory to store results between runs
BUILDDIR="build/gh-checks/clang-12/"
mkdir -p "${CURDIR}/../../${BUILDDIR}"

# Run docker with action-cxx-toolkit to check our code
docker run ${DOCKER_RUN_PARAMS} \
    -e INPUT_BUILDDIR="/github/workspace/${BUILDDIR}" \
    -e INPUT_PREBUILD_COMMAND="conan config set storage.path=/github/workspace/${BUILDDIR}/.conan" \
    -e INPUT_MAKEFLAGS='-j 4' \
    -e INPUT_CC='clang-12' \
    -e INPUT_CHECKS='build test' lucteo/action-cxx-toolkit.clang12:latest
status=$?
printStatus $status
