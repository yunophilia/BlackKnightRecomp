Test archives for `bkr_mods_test` (libarchive can write zip and 7z, but not RAR):

* `rar4.rar`: `test_read_format_rar.rar` (RAR 4, stored)
* `rar5.rar`: `test_read_format_rar5_compressed.rar` (RAR 5, compressed)

Both come from libarchive's test suite (`libarchive/test/*.rar.uu`, decoded), under
libarchive's BSD 2-clause licence (shipped in `licenses/` with the game).

Preview images for the image checks (made for this test, 64 x 48, a red top-left corner):

* `preview.png`, `preview.jpg`: decoded as they are, and truncated, corrupted and with
  oversized dimensions in the test.
