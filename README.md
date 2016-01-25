# sxplayer


## Introduction

`sxplayer` stands for Stupeflix Player, a video frames fetching library.

It can handle only one stream at a time, generally video. If audio is
requested, the frame returned will be an instant video frame containing both
amplitudes and frequencies information.

## License

LGPL v2.1, see `LICENSE`.

## Compilation, installation

The only dependency is FFmpeg.

`make` is enough to build the `libsxplayer.a`.

If you prefer a dynamic library, you use the variable `SHARED`, such as `make
SHARED=yes`.

If you need symbol debugging, you can use `make DEBUG=yes`.

Make allow options to be combinable, so `make SHARED=yes DEBUG=yes` is valid.

`make install` will install the library in `PREFIX`, which you can override,
for example using `make install PREFIX=/tmp/sxp`.


## API Usage

For API usage, refers to `sxplayer.h`.


## Development

### Infrastructure overview

```
.------------.           .-------------------------------------------------.
| sxplayer.c |           | async                                           |
+------------+           +-------------------------------------------------+
|            |  query    |                                                 |
|            |---------->|                MANAGE THREADS                   |
|            |           |          _________/  |  \_______                |
`------------'           |         /            |          \               |
                         |        /             |           \              |
                         |       v              v            v             |
                         |  .----------.  .----------.  .----------.       |
                         |  | demuxer  |  | decoder  |  | filterer |       |
                         |  +----------+  +----------+  +----------+       |
                         |  | init     |  | init     |  | init     |       |
                         |  +----------+  +----------+  +----------+       |
                         |  |          |  |          |  |          |       |
                         |  |  RUN     |  |  RUN     |  |  RUN     |       |
                         |  |          |  |          |  |          |       |
                         |  |          |  |          |  |          |       |
                         |  |       O==|==|==O    O==|==|==O    O==|==O    |
                         |  |          |  |          |  |          |       |
                         |  +----------+  +----------+  +----------+       |
                         |  | free     |  | free     |  | free     |       |
                         |  `----------'  `----------'  `----------'       |
                         |                                                 |
                         |                                                 |
                         `-------------------------------------------------'
```
