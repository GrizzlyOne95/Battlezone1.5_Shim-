# Indeo Video 5 decoder (vendored from FFmpeg)

45 of Battlezone's 52 menu clips are Indeo Video 5. Microsoft removed the Indeo
decoders from Windows years ago, so there is nothing on a modern machine that
can decode them — which is why the menu animations are missing and the
cutscenes play their soundtrack over a blank rectangle.

Rather than converting the game's files, the shim supplies the missing decoder
and registers it with Video for Windows for its own process only. That needs a
decoder that can be compiled into a 32-bit DLL with no external dependency, so
FFmpeg's is carried here directly.

## Provenance

Taken from FFmpeg release **n6.1**, `libavcodec/`:

| File | Upstream |
| --- | --- |
| `indeo5.c` | `libavcodec/indeo5.c` |
| `indeo5data.h` | `libavcodec/indeo5data.h` |
| `ivi.c` | `libavcodec/ivi.c` |
| `ivi.h` | `libavcodec/ivi.h` |
| `ivi_dsp.c` | `libavcodec/ivi_dsp.c` |
| `ivi_dsp.h` | `libavcodec/ivi_dsp.h` |

They are **unmodified except for their `#include` lines**, which now point at
`ffcompat.h`, plus one change at the end of `indeo5.c`: the `FFCodec
ff_indeo5_decoder` registration block is replaced by a plain
`ff_indeo5_decode_init` entry point, since there is no codec registry here.

Keeping the diff that small is deliberate. The decoding is not reimplemented or
"ported by hand" — the arithmetic that has to be exact stays byte-identical to
upstream, and re-vendoring a newer FFmpeg is a copy plus the same two edits.

## What is not from FFmpeg

* `ffcompat.h` / `ffcompat.c` — stands in for the slice of libavcodec/libavutil
  the decoder expects: an LE bitstream reader, a VLC decoder, allocation, and
  small `AVCodecContext`/`AVFrame` stand-ins. The header explains which parts
  have to match FFmpeg's semantics exactly and how they were derived.
* `indeo_decode.h` / `indeo_decode.c` — the shim's entry point: create a
  decoder for a given size, push one compressed frame, get YUV410P back.

## Verifying it

`tools/indeo_check` decodes a clip and compares every plane, byte for byte,
against frames produced by `ffmpeg.exe`:

```bash
ffmpeg -i clip.avi -fps_mode passthrough -pix_fmt yuv410p -f rawvideo ref.yuv
indeo_check clip.avi ref.yuv
```

At the time of vendoring this matched exactly across all 52 Indeo clips in the
game (roughly 5,300 frames), including the twelve unreadable two-byte chunks in
`movie/intro.avi` that FFmpeg also refuses.

Use `-fps_mode passthrough`: without it FFmpeg duplicates held frames to keep a
constant rate, and the comparison drifts out of alignment on `credits.avi`.

## Licence

FFmpeg's Indeo 5 decoder is **LGPL 2.1 or later**, and the per-file notices are
intact. Compiling it into `winmm.dll` makes that binary a combined work covered
by the LGPL: distribute the corresponding source (this directory, unmodified
upstream plus the edits described above) and keep users able to relink against
a modified decoder.
