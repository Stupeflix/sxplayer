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

`make` is enough to build `libsxplayer.a`.

If you prefer a dynamic library, you can use the variable `SHARED`, such as
`make SHARED=yes`.

If you need symbol debugging, you can use `make DEBUG=yes`.

Make allow options to be combinable, so `make SHARED=yes DEBUG=yes` is valid.

`make install` will install the library in `PREFIX`, which you can override,
for example using `make install PREFIX=/tmp/sxp`.


## API Usage

For API usage, refers to `sxplayer.h`.


## Development

### Infrastructure overview

```
              .                             .
              .             API             .
              .                             .
              . . . . . . . . . . . . . . . .
              .                             .   .------------.               .-----------------------------------------------------.
              .                             .   | sxplayer.c |               | control                                             |
              .                             .   +------------+               +-----------------------------------------------------+
           ===.===  sxplayer_create() ======.==>|            |===  init  ===>|                                                     |
              .                             .   |            |---  start  -->|                    MANAGE THREADS                   |
           ===.===  sxplayer_get_*frame() ==.==>|            |---  seek  --->|              _________/  |  \_______                |
  USER        .                             .   |            |---  stop  --->|             /            |          \               |
           ---.---  sxplayer_start() -------.-->|            |===  free  ===>|            /             |           \              |
           ---.---  sxplayer_seek()  -------.-->|            |               |           v              v            v             |
           ---.---  sxplayer_seek()  -------.-->|            |               |      .----------.  .----------.  .----------.       |
              .                             .   |            |               |      | demuxer  |  | decoder  |  | filterer |       |
           ===.===  sxplayer_free()  =======.==>|            |               |      +----------+  +----------+  +----------+       |
              .                             .   |            |               |      | init     |  | init     |  | init     |       |
              .                             .   `------------'               |      +----------+  +----------+  +----------+       |
              .                             .                                |      |          |  |          |  |          |       |
              .                             .                                |      |  RUN     |  |  RUN     |  |  RUN     |       |
              .                             .                                |      |          |  |          |  |          |       |
                                                                             |      |          |  |          |  |          |       |
                                                                             |   O==|==O    O==|==|==O    O==|==|==O    O==|==O    |
                                                                             |      |          |  |          |  |          |       |
                                                                             |      +----------+  +----------+  +----------+       |
                                                                             |      | free     |  | free     |  | free     |       |
                                                                             |      `----------'  `----------'  `----------'       |
                                                                             |                                                     |
                                                                             |                                                     |
                                                                             |   O=====O in control queue                          |
                                                                             |                                                     |
                                                                             |   O=====O out control queue                         |
                                                                             |                                                     |
                                                                             `-----------------------------------------------------'

O====O       message queue
-- xxx -->   async operation
== xxx ==>   sync operation
```
