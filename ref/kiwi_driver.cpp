// SPDX-License-Identifier: GPL-3.0-or-later
//
// Reference receiver driver: runs the unmodified KiwiSDR extensions/ALE_2G decoder
// (decode_ff_impl.cpp, compiled as its own translation unit with its standalone main renamed)
// on an 8000 Hz 16-bit mono WAV, exactly like its run_standalone(): blocks of 1024 samples,
// a final partial block dropped, then the end-of-input flush. Display mode ALL (every log
// line) and, with -w, gdebug = 2 (the per-word trace). Output goes to stdout with the
// reference's ANSI colours; scripts/make_vectors.py strips them.
//
// The class members are private; this translation unit reads the header with private and
// protected mapped to public to set gdebug (layout is unchanged). The decoder object has
// static storage (zero-initialised) like KiwiSDR's ale_2g_chan[] array.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "types.h"
#include "ansi.h"

// the header defines these two stand-ins under STANDALONE_TEST; the reference TU owns them
#define ext_update_get_sample_rateHz kiwi_driver_unused_rate
#define ext_send_msg kiwi_driver_unused_msg
#define private public
#define protected public
#include "decode_ff_impl.h"
#undef private
#undef protected

static bool read_wav(const char* path, std::vector<short>& out)
{
    FILE* fp = std::fopen(path, "rb");
    if (!fp) return false;
    unsigned char hdr[12], ch[8];
    bool ok = false;
    if (std::fread(hdr, 1, 12, fp) == 12 && !std::memcmp(hdr, "RIFF", 4)) {
        while (std::fread(ch, 1, 8, fp) == 8) {
            unsigned long len = ch[4] | (ch[5] << 8) | ((unsigned long)ch[6] << 16)
                                | ((unsigned long)ch[7] << 24);
            if (!std::memcmp(ch, "data", 4)) {
                std::vector<unsigned char> raw(len);
                if (std::fread(raw.data(), 1, len, fp) == len) {
                    out.resize(len / 2);
                    for (size_t i = 0; i < out.size(); ++i)
                        out[i] = (short)(raw[2 * i] | (raw[2 * i + 1] << 8));
                    ok = true;
                }
                break;
            }
            std::fseek(fp, (long)(len + (len & 1)), SEEK_CUR);
        }
    }
    std::fclose(fp);
    return ok;
}

int main(int argc, char** argv)
{
    bool words = false;
    const char* path = nullptr;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "-w")) words = true;
        else path = argv[i];
    }
    std::vector<short> pcm;
    if (!path || !read_wav(path, pcm)) {
        std::fprintf(stderr, "usage: kiwi_ref [-w] file.wav\n");
        return 2;
    }
    // zero-initialised storage, as in KiwiSDR (a member of the static ale_2g_chan[] array): the
    // reference reads command bytes it never wrote (README quirk Q5) and leaves gdebug unset
    static ale::decode_ff_impl decoder;
    ale::decode_ff_impl* d = &decoder;
    d->modem_init(0, false, 0, N_SAMP_BUF, false);
    d->set_display(ALL);
    d->gdebug = words ? 2 : 0;
    d->calc_mag = true;
    static float fbuf[N_SAMP_BUF];
    for (size_t off = 0; off + N_SAMP_BUF <= pcm.size(); off += N_SAMP_BUF) {
        for (int i = 0; i < N_SAMP_BUF; ++i) fbuf[i] = (float)(s2_t)pcm[off + i];
        d->do_modem(fbuf, N_SAMP_BUF);
    }
    d->do_modem(fbuf, 0);
    std::fflush(stdout);
    return 0;
}
