// SPDX-License-Identifier: GPL-3.0-or-later
//
// Reference transmitter for the ale-core vectors: builds ALE transmissions with the
// unmodified openALE sources (MIT, pinned in ref/PINS) and writes, per scenario,
//   <out>/<name>.json  words (24-bit), tx49 (49 bits as hex), 49 symbols per word
//   <out>/<name>.s16   8000 Hz int16 LE audio from openALE's ToneGenerator (TX_AMPLITUDE,
//                      TPDF dither of its own PRNG), phase-continuous across the scenario.
// Built by ref/build_ref.sh; not part of the library.
#include <cstdio>
#include <string>
#include <vector>

#include "Codec/ale_encoder.h"
#include "FSK/tone_generator.h"
#include "LQA/lqa_metrics.h"
#include "Protocol/Message/ale_orderwire_protocols.h"
#include "Word/ale_sequence.h"

using namespace ale;

namespace {

struct Scenario {
    std::string name;
    std::vector<ALEWord> words;
};

void add(std::vector<ALEWord>& out, const ALESequence& seq)
{
    out.insert(out.end(), seq.words().begin(), seq.words().end());
}

void add(std::vector<ALEWord>& out, const std::vector<ALEWord>& words)
{
    out.insert(out.end(), words.begin(), words.end());
}

std::vector<ALEWord> sounding(const std::string& self, bool twas, int reps)
{
    // ALEStateMachine::handle_sounding: the conclusion repeated (n + 2) times
    std::vector<ALEWord> out;
    const ALESequence c = ALESequenceBuilder::conclusion(self, twas);
    for (int i = 0; i < reps; ++i) add(out, c);
    return out;
}

std::vector<ALEWord> call(const std::string& dest, const std::string& self, int scan,
                          const LQACmdPayload* lqa, const std::string& amd)
{
    // ALEStateMachine::enqueue_call_sequence_: scanning, leading, [FROM], [LQA], [AMD],
    // conclusion
    std::vector<ALEWord> out;
    add(out, ALESequenceBuilder::scanning_call(dest, scan));
    add(out, ALESequenceBuilder::leading_call(dest));
    if (!amd.empty()) add(out, ALESequenceBuilder::from_id(self));
    if (lqa) add(out, ALESequenceBuilder::lqa_cmd(encode_lqa_cmd(*lqa)));
    if (!amd.empty()) add(out, encode_amd(amd));
    add(out, ALESequenceBuilder::conclusion(self, false));
    return out;
}

uint32_t raw24(const ALEWord& w)
{
    return (static_cast<uint32_t>(w.type) << 21) | (w.raw_payload & 0x1FFFFFu);
}

bool write(const std::string& dir, const Scenario& s)
{
    std::string json = "{\n  \"name\": \"" + s.name + "\",\n  \"words\": [\n";
    std::vector<uint8_t> symbols;
    for (size_t i = 0; i < s.words.size(); ++i) {
        const ALEWord& w = s.words[i];
        const uint64_t tx49 = w.encode();
        const SymbolFrame f = ALEEncoder::encode_tx49(tx49);
        char buf[128];
        std::snprintf(buf, sizeof buf, "    {\"word\": %u, \"tx49\": \"%013llx\", \"symbols\": [",
                      raw24(w), static_cast<unsigned long long>(tx49));
        json += buf;
        for (size_t k = 0; k < f.size(); ++k) {
            json += std::to_string(f[k]) + (k + 1 < f.size() ? ", " : "");
            symbols.push_back(f[k]);
        }
        json += std::string("]}") + (i + 1 < s.words.size() ? ",\n" : "\n");
    }
    json += "  ]\n}\n";

    std::vector<int16_t> pcm(symbols.size() * SAMPLES_PER_SYMBOL);
    ToneGenerator gen;
    gen.generate_symbols(symbols.data(), static_cast<uint32_t>(symbols.size()), pcm.data());

    FILE* fj = std::fopen((dir + "/" + s.name + ".json").c_str(), "w");
    FILE* fa = std::fopen((dir + "/" + s.name + ".s16").c_str(), "wb");
    if (!fj || !fa) return false;
    std::fputs(json.c_str(), fj);
    for (int16_t v : pcm) {
        const unsigned char le[2] = {static_cast<unsigned char>(v & 0xFF),
                                     static_cast<unsigned char>((v >> 8) & 0xFF)};
        std::fwrite(le, 1, 2, fa);
    }
    std::fclose(fj);
    std::fclose(fa);
    return true;
}

}  // namespace

int main(int argc, char** argv)
{
    if (argc != 2) {
        std::fprintf(stderr, "usage: openale_vectors <out-dir>\n");
        return 2;
    }
    const std::string dir = argv[1];
    LQACmdPayload lqa;
    lqa.ka1 = true;
    lqa.mp = 2;
    lqa.sinad = 17;
    lqa.ber = 5;

    std::vector<Scenario> sc;
    sc.push_back({"sound_twas_dg6fl", sounding("DG6FL", true, 4)});
    sc.push_back({"sound_tis_dl1abc", sounding("DL1ABC", false, 3)});
    sc.push_back({"sound_twas_long", sounding("WAVEOWLTESTSTN", true, 3)});
    sc.push_back({"call_dl1abc", call("DL1ABC", "DG6FL", 2, nullptr, "")});
    sc.push_back({"response_dg6fl", {}});
    add(sc.back().words, ALESequenceBuilder::response("DG6FL", "DL1ABC", false));
    sc.push_back({"ack_dl1abc", {}});
    add(sc.back().words, ALESequenceBuilder::ack("DL1ABC", "DG6FL", false));
    sc.push_back({"termination_dl1abc", {}});
    add(sc.back().words, ALESequenceBuilder::termination("DL1ABC", "DG6FL"));
    sc.push_back({"call_lqa", call("DL1ABC", "DG6FL", 1, &lqa, "")});
    sc.push_back({"call_amd", call("DL1ABC", "DG6FL", 1, nullptr, "HELLO FROM WAVE OWL 73")});
    sc.push_back({"allcall", call("@?@", "DG6FL", 1, nullptr, "")});
    sc.push_back({"anycall", call("@@?", "DG6FL", 1, nullptr, "")});
    sc.push_back({"netcall_hfn", call("HFN", "DG6FL", 2, nullptr, "")});
    for (const Scenario& s : sc)
        if (!write(dir, s)) {
            std::fprintf(stderr, "openale_vectors: cannot write %s\n", s.name.c_str());
            return 1;
        }
    return 0;
}
