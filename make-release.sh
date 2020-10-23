#!/bin/sh

#
# Release process:
# 1. on a clean git state, run this script with the new version as argument
# 2. check the last commit and tag
# 3. git push && git push --tags
#

set -eu

if [ $# -ne 1 ]; then
	echo "Usage $0 <major.minor.micro>"
	exit 1
fi

cd "$(dirname $0)"

if ! git diff-index --quiet HEAD; then
	echo "Git index is not clean"
	exit 1
fi

set -x
VERSION="$1"
echo "$VERSION" > VERSION
git add VERSION
git commit -m "Release $VERSION"
git tag "v$VERSION"
