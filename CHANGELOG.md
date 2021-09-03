# Changelog
All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]
### Fixed
- Fix Windows static builds

## [9.9.0] - 2021-06-10
### Added
- Automatic software pixel format selection
- New datap and linesizep fields to sxplayer frame
- NV12, YUV420P, YUV422P, YUV444P support
- P010LE, YUV420PLE, YUV422P10LE, YUV444P10LE support

### Fixed
- Multiple audio frames without PTS
- Removed inoffensive seek error logging on images (regression)

## [9.8.1] - 2021-03-25
### Fixed
- Fixed compiler warnings on Windows

## [9.8.0] - 2021-03-24
### Changed
- Remove pthread dependency on Windows

## [9.7.0] - 2020-12-10
### Added
- Official MSVC support

## [9.6.0] - 2020-11-07
### Added
- This Changelog

### Changed
- Switch build system from GNU/Make to [Meson](https://mesonbuild.com/)

### Fixed
- Fixed assert on image seeking
