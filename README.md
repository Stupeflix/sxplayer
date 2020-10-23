# sxplayer

![tests Linux](https://github.com/stupeflix/sxplayer/workflows/tests%20Linux/badge.svg)
![tests Mac](https://github.com/stupeflix/sxplayer/workflows/tests%20Mac/badge.svg)

## Introduction

`sxplayer` stands for Stupeflix Player, a video frames fetching library.

It can handle only one stream at a time, generally video. If audio is
requested, the frame returned will be an instant video frame containing both
amplitudes and frequencies information.

## License

LGPL v2.1, see `LICENSE`.

## Compilation, installation

```sh
meson setup builddir
meson compile -C builddir
meson install -C builddir
```

`meson configure` can be used to list the available options. See the [Meson
documentation][meson-doc] for more information.

[meson-doc]: https://mesonbuild.com/Quick-guide.html#compiling-a-meson-project


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
