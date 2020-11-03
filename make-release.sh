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

if ! sed --version 2>/dev/null | grep -m1 -q GNU; then
	echo "GNU/sed is required"
	exit 1
fi

set -x
VERSION="$1"
sed "/^## \[Unreleased\]/a \\\n## [$VERSION] - $(date -I)" -i CHANGELOG.md
echo "$VERSION" > VERSION
git add VERSION CHANGELOG.md
git commit -m "Release $VERSION"
git tag "v$VERSION"
