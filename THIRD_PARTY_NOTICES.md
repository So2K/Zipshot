# Third party notices

Zipshot is distributed under GNU GPL version 3 or later. See [LICENSE](LICENSE).
The implementation follows ShareX behavior and includes resources from ShareX;
this is an independent project and is not endorsed by the ShareX team.

* **ShareX** — ShareX Team and contributors, GPL. The sounds, crosshair cursor,
  toolbar resources and behavior references originate from ShareX 18.0.1.
  Source and license: https://github.com/ShareX/ShareX/tree/v18.0.1
* **Fugue Icons** — copyright Yusuke Kamiyamane, Creative Commons Attribution 3.0.
  Some icons are modified for the dark theme. https://p.yusukekamiyamane.com/
  License: https://creativecommons.org/licenses/by/3.0/

Optional sticker packs are downloaded separately by `tools/fetch_stickers.py`;
they are not included in the executable or release archive. Each downloaded pack
gets upstream notices. Fluent Emoji is MIT (Microsoft); Twemoji graphics and Noto
animated emoji are CC BY 4.0. Noto Emoji's upstream README describes most image
resources as Apache 2.0, while its current root LICENSE contains SIL OFL 1.1.
That upstream inconsistency has not been reconciled here; verify the license for
the exact assets/revision before redistributing that optional pack. It is not
included in this release. See the downloader for upstream URLs and pack handling.

FFmpeg is a separate optional dependency. It is not bundled. Its license depends
on the selected build: https://ffmpeg.org/legal.html

The comparison video uses original synthetic fixtures, original synthesized
audio, and captures of Zipshot's real offscreen editor. ShareX is a trademark of
its respective owner. Benchmark source/provenance accompanies the measurements.
