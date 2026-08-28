# Bundled component licenses

RetroTuner3DS includes source, headers, static libraries, and resources from
other open-source projects. This directory preserves the license material
retrieved from the exact source revisions used by the repository.

| Component | Bundled material | Source revision | License material |
| --- | --- | --- | --- |
| Video player for 3DS | Base application source and resources | [9a0172351db2df3c06f52a3dfeb658c47cabddfd](https://github.com/Core-2-Extreme/Video_player_for_3DS/commit/9a0172351db2df3c06f52a3dfeb658c47cabddfd) | GPL-3.0-or-later; repository root [LICENSE](../LICENSE) |
| libctru | libctru.a and headers | [7e3e1c1be4217093db1baca96d6dcf0db23588f4](https://github.com/Core-2-Extreme/libctru_custom/commit/7e3e1c1be4217093db1baca96d6dcf0db23588f4) | zlib; [libctru.txt](libctru.txt) |
| Citro3D | libcitro3d.a, headers, and Tex3DS header | [464b01141f7acf2ae8564a2c0a936b9c17c13219](https://github.com/Core-2-Extreme/citro3d_custom/commit/464b01141f7acf2ae8564a2c0a936b9c17c13219) | zlib; [citro3d.txt](citro3d.txt) |
| Citro2D | libcitro2d.a and headers | [7c7275903602fab9f7165e20c486040b8ec0d48c](https://github.com/Core-2-Extreme/citro2d_custom/commit/7c7275903602fab9f7165e20c486040b8ec0d48c) | zlib; [citro2d.txt](citro2d.txt) |
| x264 | libx264.a and headers | [c24e06c2e184345ceb33eb20a15d1024d9fd3497](https://github.com/Core-2-Extreme/x264_for_3DS/commit/c24e06c2e184345ceb33eb20a15d1024d9fd3497) | GPL-2.0; [x264.txt](x264.txt) |
| LAME | libmp3lame.a and headers | [f416c19b3140a8610507ebb60ac7cd06e94472b8](https://github.com/Core-2-Extreme/libmp3lame_for_3DS/commit/f416c19b3140a8610507ebb60ac7cd06e94472b8) | LGPL-2.0; [libmp3lame.txt](libmp3lame.txt) |
| dav1d | libdav1d.a and headers | [c40d9602629d39ae63bedd50c31fd926fa5eb51e](https://github.com/Core-2-Extreme/dav1d_for_3DS/commit/c40d9602629d39ae63bedd50c31fd926fa5eb51e) | BSD-2-Clause; [dav1d.txt](dav1d.txt) |
| FFmpeg | libavcodec, libavformat, libavutil, libswresample, and libswscale archives and headers | [dab24a843203b2b191f40e39907fb146f688ec5c](https://github.com/Core-2-Extreme/FFmpeg_for_3DS/commit/dab24a843203b2b191f40e39907fb146f688ec5c) | GPLv3 build; [FFmpeg-GPLv3.txt](FFmpeg-GPLv3.txt) |
| zlib | libz.a and headers | [da607da739fa6047df13e66a2af6b8bec7c2a498](https://github.com/Core-2-Extreme/zlib_for_3DS/commit/da607da739fa6047df13e66a2af6b8bec7c2a498) | zlib; [zlib.txt](zlib.txt) |
| Mbed TLS | Mbed TLS archives and headers | [ecf77d19bfc2b2630cccabb033ab7227ff6b0beb](https://github.com/Core-2-Extreme/mbedtls_for_3DS/commit/ecf77d19bfc2b2630cccabb033ab7227ff6b0beb) | Apache-2.0 OR GPL-2.0-or-later; [Mbed-TLS.txt](Mbed-TLS.txt), [Apache-2.0.txt](Apache-2.0.txt), and [x264.txt](x264.txt) for the GPLv2 text |
| TF-PSA-Crypto | libtfpsacrypto.a and headers | [961565a777395a8098342ab1e92ced8fb3ab5681](https://github.com/Core-2-Extreme/TF-PSA-Crypto_for_3ds/commit/961565a777395a8098342ab1e92ced8fb3ab5681) | Apache-2.0 OR GPL-2.0-or-later; [TF-PSA-Crypto.txt](TF-PSA-Crypto.txt) and [Apache-2.0.txt](Apache-2.0.txt) |
| nghttp2 | libnghttp2.a and headers | [68cb6900fde14c77f0cd7add0e094a862960eb99](https://github.com/Core-2-Extreme/nghttp2_for_3DS/commit/68cb6900fde14c77f0cd7add0e094a862960eb99) | MIT; [nghttp2.txt](nghttp2.txt) |
| curl | libcurl.a and headers | [555f7e95e296995fb0fd1cc227ebb45912845eb5](https://github.com/Core-2-Extreme/curl_for_3DS/commit/555f7e95e296995fb0fd1cc227ebb45912845eb5) | curl license; [curl.txt](curl.txt) |
| stb_image / stb_image_write | Headers | [8c3b4f1a58aa77f9d020a3c9f53847e231e37fc5](https://github.com/Core-2-Extreme/stb_for_3DS/commit/8c3b4f1a58aa77f9d020a3c9f53847e231e37fc5) | MIT OR public domain; [stb.txt](stb.txt) |
| jsmn | Header | [25647e692c7906b96ffd2b05ca54c097948e879c](https://github.com/Core-2-Extreme/jsmn_for_3DS/commit/25647e692c7906b96ffd2b05ca54c097948e879c) | MIT; [jsmn.txt](jsmn.txt) |
| Mozilla CA certificate store | romfs/gfx/cert/cacert.pem, extracted July 2, 2024 | [curl CA Extract](https://curl.se/docs/caextract.html) | MPL-2.0; [Mozilla-Public-License-2.0.txt](Mozilla-Public-License-2.0.txt) |

The repository's `library/` directory records how each pinned source revision
was configured for Nintendo 3DS. The project source snapshot made by
`scripts/package-release.sh` contains this repository only; it is not complete
corresponding source for the prebuilt third-party libraries. Tagged public
packages therefore require a separately prepared and audited third-party source
archive containing the revisions listed above. The packaging script checks that
an archive was supplied, but cannot verify its contents. Copyright notices
embedded in individual source and header files remain in effect.

This inventory documents the repository as audited on August 22, 2026. When a
vendored component is upgraded, update its pinned revision and corresponding
license material in the same change.

The pinned links above identify the audited revisions; they are not themselves
a source bundle or a substitute for any license-required source distribution.
Anyone distributing a binary is responsible for satisfying the applicable
source-code and notice requirements.
