// Test unitari per annexb.h/.cpp — nessuna dipendenza esterna (ne' framework di test ne' SDK
// NDI), cosi' da poter girare in CI senza il proprietario NDI Advanced SDK (code review:
// Codex, fase 2 - "questa parte puo' essere compilata e testata senza SDK proprietario").
//
// Harness minimale: macro CHECK/CHECK_EQ + auto-registrazione delle TEST() via costruttori
// statici. Vedi main() in fondo per l'esecuzione.

#include "../annexb.h"

#include <cstdint>
#include <cstdio>
#include <vector>

static int g_checks = 0;
static int g_failures = 0;

#define CHECK(cond) do { \
    g_checks++; \
    if (!(cond)) { \
        g_failures++; \
        fprintf(stderr, "  FAIL %s:%d: CHECK(%s)\n", __FILE__, __LINE__, #cond); \
    } \
} while (0)

#define CHECK_EQ(a, b) do { \
    g_checks++; \
    if (!((a) == (b))) { \
        g_failures++; \
        fprintf(stderr, "  FAIL %s:%d: CHECK_EQ(%s, %s)\n", __FILE__, __LINE__, #a, #b); \
    } \
} while (0)

using TestFn = void (*)();
struct TestCase { const char* name; TestFn fn; };

static std::vector<TestCase>& registry() {
    static std::vector<TestCase> r;
    return r;
}

struct Registrar {
    Registrar(const char* name, TestFn fn) { registry().push_back({name, fn}); }
};

#define TEST(name) \
    static void test_##name(); \
    static Registrar registrar_##name(#name, test_##name); \
    static void test_##name()

// --- helper --------------------------------------------------------------------------------

static void append_start_code(std::vector<uint8_t>* out, int len) {
    if (len == 4) out->push_back(0);
    out->push_back(0);
    out->push_back(0);
    out->push_back(1);
}

// ============================================================================================
// NalReader
// ============================================================================================

// Copre: "start code a tre e quattro byte" dalla checklist della code review.
TEST(nalreader_3_and_4_byte_start_codes) {
    annexb::NalReader r;
    std::vector<uint8_t> stream;
    append_start_code(&stream, 4);
    stream.push_back(0x67); stream.push_back(0xAA); // NAL 1 (delimitato da start code a 4 byte)
    append_start_code(&stream, 3);
    stream.push_back(0x68); stream.push_back(0xBB); // NAL 2 (delimitato da start code a 3 byte)
    append_start_code(&stream, 4);
    stream.push_back(0x65); stream.push_back(0xCC); // NAL 3 (terminatore per chiudere NAL 2)
    r.feed(stream);

    std::vector<uint8_t> nal;
    CHECK(r.next_nal(&nal));
    CHECK_EQ(nal.size(), (size_t)2);
    CHECK_EQ(nal[0], 0x67);

    CHECK(r.next_nal(&nal));
    CHECK_EQ(nal.size(), (size_t)2);
    CHECK_EQ(nal[0], 0x68);

    CHECK(!r.next_nal(&nal)); // NAL 3 non ancora chiuso da un quarto start code
    CHECK(r.flush(&nal));
    CHECK_EQ(nal[0], 0x65);
}

// Copre: "input diviso in chunk di dimensioni arbitrarie".
TEST(nalreader_arbitrary_chunking_matches_single_feed) {
    std::vector<uint8_t> stream;
    append_start_code(&stream, 4);
    for (uint8_t b : {(uint8_t)0x67, (uint8_t)0x01, (uint8_t)0x02, (uint8_t)0x03}) stream.push_back(b);
    append_start_code(&stream, 4);
    for (uint8_t b : {(uint8_t)0x65, (uint8_t)0x04, (uint8_t)0x05}) stream.push_back(b);
    append_start_code(&stream, 4);
    stream.push_back(0x01); // NAL finale minimale (recuperabile solo con flush())

    annexb::NalReader ref;
    ref.feed(stream);
    std::vector<std::vector<uint8_t>> ref_nals;
    std::vector<uint8_t> n;
    while (ref.next_nal(&n)) ref_nals.push_back(n);
    if (ref.flush(&n)) ref_nals.push_back(n);

    // Stesso stream, alimentato un byte alla volta: il risultato deve essere identico
    // indipendentemente da come i dati arrivano (simula read() che ritorna chunk arbitrari).
    annexb::NalReader chunked;
    std::vector<std::vector<uint8_t>> chunked_nals;
    for (uint8_t b : stream) {
        chunked.feed(&b, 1);
        while (chunked.next_nal(&n)) chunked_nals.push_back(n);
    }
    if (chunked.flush(&n)) chunked_nals.push_back(n);

    CHECK_EQ(ref_nals.size(), chunked_nals.size());
    for (size_t i = 0; i < ref_nals.size() && i < chunked_nals.size(); i++)
        CHECK(ref_nals[i] == chunked_nals[i]);
}

// Copre: "ultimo NAL a EOF" (il bug corretto in fase 1: prima andava perso).
TEST(nalreader_flush_recovers_last_nal_at_eof) {
    annexb::NalReader r;
    std::vector<uint8_t> stream;
    append_start_code(&stream, 4);
    stream.push_back(0x67); stream.push_back(0xAA);
    r.feed(stream);

    std::vector<uint8_t> nal;
    CHECK(!r.next_nal(&nal)); // nessun secondo start code: non ancora "completo"
    CHECK(r.flush(&nal));
    CHECK_EQ(nal.size(), (size_t)2);
    CHECK_EQ(nal[0], 0x67);
    CHECK(!r.flush(&nal)); // buffer ormai vuoto: flush ripetuto non deve inventarsi niente
}

// Copre: "dati troncati o malformati" (nessuno start code nel flusso).
TEST(nalreader_no_start_code_never_returns_a_nal) {
    annexb::NalReader r;
    std::vector<uint8_t> garbage = {0x12, 0x34, 0x56, 0x78, 0x9A};
    r.feed(garbage);
    std::vector<uint8_t> nal;
    CHECK(!r.next_nal(&nal));
    CHECK(!r.flush(&nal));
}

// ============================================================================================
// FrameAssembler
// ============================================================================================

// Copre: "SPS e PPS ripetuti" — l'extradata non deve restare "stale" al giro successivo.
TEST(assembler_repeated_sps_pps_updates_extradata) {
    annexb::FrameAssembler a;
    annexb::AssembledFrame out;

    std::vector<uint8_t> sps1 = {0x07, 0x11, 0x22};
    std::vector<uint8_t> pps1 = {0x08, 0x33};
    std::vector<uint8_t> idr  = {0x05, 0x99};

    CHECK(!a.feed_nal(sps1, 30, 1, &out));
    CHECK(!a.feed_nal(pps1, 30, 1, &out));
    CHECK(a.feed_nal(idr, 30, 1, &out));
    CHECK(out.is_keyframe);
    CHECK(!out.extra_data.empty());
    std::vector<uint8_t> first_extradata = out.extra_data;

    std::vector<uint8_t> sps2 = {0x07, 0x44, 0x55, 0x66}; // SPS "diverso" (piu' lungo)
    std::vector<uint8_t> pps2 = {0x08, 0x77};
    CHECK(!a.feed_nal(sps2, 30, 1, &out));
    CHECK(!a.feed_nal(pps2, 30, 1, &out));
    CHECK(a.feed_nal(idr, 30, 1, &out));
    CHECK(out.is_keyframe);
    CHECK(!(out.extra_data == first_extradata));
}

// Copre: "IDR senza parametri disponibili" — non deve essere marcato come keyframe.
TEST(assembler_idr_without_params_is_not_a_keyframe) {
    annexb::FrameAssembler a;
    annexb::AssembledFrame out;
    std::vector<uint8_t> idr = {0x05, 0x99};
    CHECK(a.feed_nal(idr, 30, 1, &out));
    CHECK(!out.is_keyframe);
    CHECK(out.extra_data.empty());
}

// Copre: "frame con piu' slice". Limite noto (vedi annexb.h): l'assembler NON ricompone piu'
// slice in un'unica access unit. Questo test fissa il comportamento ATTUALE, cosi' che un
// eventuale supporto multi-slice futuro sia una scelta esplicita e non una regressione
// silenziosa scoperta troppo tardi.
TEST(assembler_multi_slice_known_limitation_emits_separate_frames) {
    annexb::FrameAssembler a;
    annexb::AssembledFrame out;
    std::vector<uint8_t> sps = {0x07, 0x11};
    std::vector<uint8_t> pps = {0x08, 0x22};
    a.feed_nal(sps, 30, 1, &out);
    a.feed_nal(pps, 30, 1, &out);

    std::vector<uint8_t> slice1 = {0x01, 0xAA}; // due slice dello stesso frame reale
    std::vector<uint8_t> slice2 = {0x01, 0xBB};
    CHECK(a.feed_nal(slice1, 30, 1, &out));
    CHECK_EQ(a.frames_emitted(), (uint64_t)1);
    CHECK(a.feed_nal(slice2, 30, 1, &out));
    CHECK_EQ(a.frames_emitted(), (uint64_t)2); // atteso oggi: 2 frame invece di 1 solo
}

// Copre: "dati troncati o malformati" lato assembler (NAL vuoto).
TEST(assembler_empty_nal_is_ignored_without_crashing) {
    annexb::FrameAssembler a;
    annexb::AssembledFrame out;
    std::vector<uint8_t> empty;
    CHECK(!a.feed_nal(empty, 30, 1, &out));
}

// Copre: "sequenze molto grandi e possibili overflow" — un NAL di alcuni MB e molte migliaia
// di frame in sequenza, verificando che pts/frame_index avanzino in modo monotono e coerente.
TEST(assembler_large_nal_and_long_sequence) {
    annexb::FrameAssembler a;
    annexb::AssembledFrame out;
    std::vector<uint8_t> sps = {0x07, 0x11};
    std::vector<uint8_t> pps = {0x08, 0x22};
    a.feed_nal(sps, 30, 1, &out);
    a.feed_nal(pps, 30, 1, &out);

    std::vector<uint8_t> big_slice;
    big_slice.push_back(0x01);
    big_slice.resize(1 + 4 * 1024 * 1024, 0xEE);
    CHECK(a.feed_nal(big_slice, 30, 1, &out));
    CHECK_EQ(out.data.size(), big_slice.size() + 4 /* start code */);

    std::vector<uint8_t> slice = {0x01, 0x00};
    int64_t prev_pts = out.pts_hns;
    for (int i = 0; i < 10000; i++) {
        CHECK(a.feed_nal(slice, 30, 1, &out));
        CHECK(out.pts_hns >= prev_pts);
        prev_pts = out.pts_hns;
    }
    // SPS/PPS non emettono frame: solo big_slice + i 10000 slice del loop contano.
    CHECK_EQ(a.frames_emitted(), (uint64_t)(1 + 10000));
}

// ============================================================================================
// compute_hns
// ============================================================================================

// Copre: "frame rate frazionari, per esempio 30000/1001".
TEST(compute_hns_fractional_frame_rate) {
    int64_t hns0 = annexb::compute_hns(0, 30000, 1001);
    int64_t hns1 = annexb::compute_hns(1, 30000, 1001);
    CHECK_EQ(hns0, (int64_t)0);
    // Atteso: 1001 * 10'000'000 / 30'000 ~= 333'666.67 hns
    CHECK(hns1 > 333600 && hns1 < 333700);
}

TEST(compute_hns_integer_frame_rate) {
    // A 30/1 fps un frame dura esattamente 10'000'000/30 = 333'333.33 hns.
    CHECK_EQ(annexb::compute_hns(0, 30, 1), (int64_t)0);
    CHECK_EQ(annexb::compute_hns(30, 30, 1), (int64_t)10000000); // 30 frame = esattamente 1s
}

// ============================================================================================

int main() {
    int failed_tests = 0;
    for (const auto& tc : registry()) {
        const int before = g_failures;
        tc.fn();
        if (g_failures > before) {
            failed_tests++;
            fprintf(stderr, "[FAIL] %s\n", tc.name);
        } else {
            fprintf(stderr, "[ OK ] %s\n", tc.name);
        }
    }
    fprintf(stderr, "\n%d check(s), %d failure(s), %d/%zu test(s) failed\n",
            g_checks, g_failures, failed_tests, registry().size());
    return failed_tests == 0 ? 0 : 1;
}
