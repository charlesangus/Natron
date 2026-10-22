# Camera raw fixtures

Sample raw files for the native Read's rawspeed decoder and the raw nodes. They come from the
[raw.pixls.us](https://raw.pixls.us/) sample archive, which publishes every sample under the
[CC0 1.0](https://creativecommons.org/publicdomain/zero/1.0/) public domain dedication.

The files are large, so Git LFS tracks them (`.gitattributes`). A checkout without LFS gets
pointer files instead, and the tests that read them fail. Run `git lfs pull` to fetch them. CI
checkouts that run ctest or the smoke tests set `lfs: true`.

| File | Camera | Layout | Source | Licence | Size (bytes) | SHA-256 |
| --- | --- | --- | --- | --- | --- | --- |
| `canon-eos-5d-mark-iii.cr2` | Canon EOS 5D Mark III | Bayer CFA, CR2 | [raw.pixls.us/data/Canon/Canon EOS 5D Mark III](https://raw.pixls.us/data/Canon/Canon%20EOS%205D%20Mark%20III/) | CC0 1.0 | 25965009 | `ec069b178b9383d80c72f8cdc1b79bd54ca451e7c20793b109d8ad73bef6bdf6` |
| `fujifilm-x-pro2-14bit-uncompressed.raf` | Fujifilm X-Pro2 | X-Trans CFA, 14-bit uncompressed RAF | [raw.pixls.us/data/Fujifilm/X-Pro2](https://raw.pixls.us/data/Fujifilm/X-Pro2/) | CC0 1.0 | 50622976 | `8cc12566e5c0fc676b713bb1612ce0510d47b1dc691e9a5a0bcf3799ad9b8af1` |
| `canon-eos-5d-mark-iii-linear-rgb.dng` | Canon EOS 5D Mark III, converted by Adobe DNG Converter 9.8 | Linear (demosaiced) RGB DNG, no CFA | [raw.pixls.us/data/Canon/Canon EOS 5D Mark III](https://raw.pixls.us/data/Canon/Canon%20EOS%205D%20Mark%20III/) | CC0 1.0 | 63674890 | `357e664b2a59da2bfa1e6d729be7314e5efdb4ea8610b9c4fa37546437540686` |

The files are renamed from the archive's names. The SHA-256 identifies each one.
