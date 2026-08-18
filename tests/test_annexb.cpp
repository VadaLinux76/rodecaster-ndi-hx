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

// Costruisce un NAL slice minimale con un first_mb_in_slice specifico codificato come ue(v)
// (Tabella 9-2 H.264). Copre solo i valori piccoli usati nei test.
static std::vector<uint8_t> make_slice(int nal_type, uint32_t first_mb, uint8_t tail_byte = 0xAA) {
    uint8_t ue_byte;
    switch (first_mb) {
        case 0: ue_byte = 0x80; break; // ue(v) "1"
        case 1: ue_byte = 0x40; break; // ue(v) "010"
        case 2: ue_byte = 0x60; break; // ue(v) "011"
        default: ue_byte = 0x80; break;
    }
    return {(uint8_t)nal_type, ue_byte, tail_byte};
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
// RbspBitReader / parse_first_mb_in_slice
// ============================================================================================

TEST(rbsp_bitreader_removes_emulation_prevention_bytes) {
    // RBSP "vera" prima dell'escaping: AB 00 00 01 CD (contiene "00 00 01", che nei NAL H.264
    // e' sempre preceduto da uno 0x03 di stuffing per non essere confuso con uno start code).
    std::vector<uint8_t> encoded = {0xAB, 0x00, 0x00, 0x03, 0x01, 0xCD};
    annexb::RbspBitReader r(encoded.data(), encoded.size());
    uint32_t v;
    CHECK(r.read_bits(8, &v)); CHECK_EQ(v, 0xABu);
    CHECK(r.read_bits(8, &v)); CHECK_EQ(v, 0x00u);
    CHECK(r.read_bits(8, &v)); CHECK_EQ(v, 0x00u);
    CHECK(r.read_bits(8, &v)); CHECK_EQ(v, 0x01u); // lo 0x03 e' stato rimosso automaticamente
    CHECK(r.read_bits(8, &v)); CHECK_EQ(v, 0xCDu);
    CHECK(!r.read_bits(8, &v)); // dati finiti
}

TEST(rbsp_bitreader_ue_golomb_table) {
    // Tabella 9-2 H.264: 0->"1", 1->"010", 2->"011", 3->"00100", 4->"00101".
    struct Case { std::vector<uint8_t> bytes; uint32_t expected; };
    const Case cases[] = {
        {{0x80}, 0},             // "1"
        {{0x40}, 1},             // "010"
        {{0x60}, 2},             // "011"
        {{0x24, 0x00}, 3},       // "00100"
        {{0x28, 0x00}, 4},       // "00101"
    };
    for (const auto& c : cases) {
        annexb::RbspBitReader r(c.bytes.data(), c.bytes.size());
        uint32_t v = 0xFFFFFFFFu;
        CHECK(r.read_ue(&v));
        CHECK_EQ(v, c.expected);
    }
}

TEST(parse_first_mb_in_slice_common_values) {
    uint32_t v;
    CHECK(annexb::parse_first_mb_in_slice(make_slice(1, 0), &v));
    CHECK_EQ(v, 0u);
    CHECK(annexb::parse_first_mb_in_slice(make_slice(1, 1), &v));
    CHECK_EQ(v, 1u);
    CHECK(annexb::parse_first_mb_in_slice(make_slice(1, 2), &v));
    CHECK_EQ(v, 2u);
}

TEST(parse_first_mb_in_slice_too_short_fails_gracefully) {
    uint32_t v;
    std::vector<uint8_t> too_short = {0x41}; // solo l'header NAL, nessun bit di slice_header
    CHECK(!annexb::parse_first_mb_in_slice(too_short, &v));
    std::vector<uint8_t> empty_nal;
    CHECK(!annexb::parse_first_mb_in_slice(empty_nal, &v));
}

// ============================================================================================
// FrameAssembler
// ============================================================================================

// Copre: "SPS e PPS ripetuti" — l'extradata non deve restare "stale" al giro successivo.
// Nota sulla latenza: feed_nal() restituisce l'access unit PRECEDENTE quando ne inizia uno
// nuovo (vedi annexb.h), quindi qui servono due IDR e un flush() finale per osservare entrambi
// gli extradata.
TEST(assembler_repeated_sps_pps_updates_extradata) {
    annexb::FrameAssembler a;
    annexb::AssembledFrame out;

    std::vector<uint8_t> sps1 = {0x07, 0x11, 0x22};
    std::vector<uint8_t> pps1 = {0x08, 0x33};
    std::vector<uint8_t> idr1 = make_slice(5, 0);

    CHECK(!a.feed_nal(sps1, 30, 1, &out));
    CHECK(!a.feed_nal(pps1, 30, 1, &out));
    CHECK(!a.feed_nal(idr1, 30, 1, &out)); // apre il primo AU, niente ancora da emettere

    std::vector<uint8_t> sps2 = {0x07, 0x44, 0x55, 0x66}; // SPS "diverso" (piu' lungo)
    std::vector<uint8_t> pps2 = {0x08, 0x77};
    std::vector<uint8_t> idr2 = make_slice(5, 0);
    CHECK(!a.feed_nal(sps2, 30, 1, &out));
    CHECK(!a.feed_nal(pps2, 30, 1, &out));
    CHECK(a.feed_nal(idr2, 30, 1, &out)); // chiude e restituisce il primo AU (con extradata1)
    CHECK(out.is_keyframe);
    std::vector<uint8_t> first_extradata = out.extra_data;
    CHECK(!first_extradata.empty());

    CHECK(a.flush(30, 1, &out)); // restituisce il secondo AU (con extradata2)
    CHECK(out.is_keyframe);
    CHECK(!(out.extra_data == first_extradata));
}

// Copre: "IDR senza parametri disponibili" — non deve essere marcato come keyframe.
TEST(assembler_idr_without_params_is_not_a_keyframe) {
    annexb::FrameAssembler a;
    annexb::AssembledFrame out;
    CHECK(!a.feed_nal(make_slice(5, 0), 30, 1, &out)); // apre l'AU, niente da emettere ancora
    CHECK(a.flush(30, 1, &out));
    CHECK(!out.is_keyframe); // niente SPS/PPS ancora: non va marcato come keyframe
    CHECK(out.extra_data.empty());
}

// Copre: "frame con piu' slice", risolto in fase 3. Due slice con first_mb_in_slice 0 poi !=0
// (quindi la seconda e' una continuazione) devono confluire in UN SOLO access unit.
TEST(assembler_multi_slice_merged_into_one_access_unit) {
    annexb::FrameAssembler a;
    annexb::AssembledFrame out;
    std::vector<uint8_t> sps = {0x07, 0x11};
    std::vector<uint8_t> pps = {0x08, 0x22};
    a.feed_nal(sps, 30, 1, &out);
    a.feed_nal(pps, 30, 1, &out);

    std::vector<uint8_t> f1_slice1 = make_slice(1, 0, 0xAA); // apre il frame 1
    std::vector<uint8_t> f1_slice2 = make_slice(1, 1, 0xBB); // continuazione dello stesso frame
    CHECK(!a.feed_nal(f1_slice1, 30, 1, &out));
    CHECK(!a.feed_nal(f1_slice2, 30, 1, &out));
    CHECK_EQ(a.frames_emitted(), (uint64_t)0); // ancora nulla emesso: il frame 1 e' ancora aperto

    std::vector<uint8_t> f2_slice1 = make_slice(1, 0, 0xCC); // apre il frame 2, chiude il frame 1
    CHECK(a.feed_nal(f2_slice1, 30, 1, &out));
    CHECK_EQ(a.frames_emitted(), (uint64_t)1);
    // Il frame 1 restituito deve contenere ENTRAMBE le slice (Annex-B: start code + NAL x2).
    const size_t expected_min = f1_slice1.size() + f1_slice2.size() + 4 + 4;
    CHECK(out.data.size() == expected_min);

    CHECK(a.flush(30, 1, &out)); // il frame 2 (una sola slice) resta in sospeso fino a qui
    CHECK_EQ(a.frames_emitted(), (uint64_t)2);
    CHECK(out.data.size() == f2_slice1.size() + 4);
}

// Copre lo stesso scenario di sopra ma usando un Access Unit Delimiter invece di affidarsi a
// first_mb_in_slice: un AUD deve forzare l'apertura di un nuovo AU alla slice successiva a
// prescindere da cosa dice first_mb_in_slice.
TEST(assembler_aud_forces_new_access_unit) {
    annexb::FrameAssembler a;
    annexb::AssembledFrame out;
    std::vector<uint8_t> aud = {0x09, 0xF0}; // NAL tipo 9 = Access Unit Delimiter

    CHECK(!a.feed_nal(make_slice(1, 0), 30, 1, &out)); // apre il frame 1
    CHECK(!a.feed_nal(aud, 30, 1, &out));               // AUD: la prossima slice apre un nuovo AU
    // first_mb_in_slice=1 normalmente significherebbe "continuazione", ma l'AUD vince comunque.
    CHECK(a.feed_nal(make_slice(1, 1), 30, 1, &out));
    CHECK_EQ(a.frames_emitted(), (uint64_t)1);
}

// Copre: "dati troncati o malformati" lato assembler — un NAL slice troppo corto per contenere
// first_mb_in_slice viene trattato, per prudenza, come inizio di un nuovo access unit: chiude
// (e restituisce) l'AU precedente invece di rischiare di fondervi dati corrotti.
TEST(assembler_malformed_slice_closes_previous_au) {
    annexb::FrameAssembler a;
    annexb::AssembledFrame out;
    CHECK(!a.feed_nal(make_slice(1, 0), 30, 1, &out)); // apre il frame 1 (valido)

    std::vector<uint8_t> too_short = {0x01}; // solo l'header NAL: parse_first_mb_in_slice fallisce
    CHECK(a.feed_nal(too_short, 30, 1, &out)); // chiude e restituisce il frame 1
    CHECK_EQ(a.frames_emitted(), (uint64_t)1);
}

TEST(assembler_empty_nal_is_ignored_without_crashing) {
    annexb::FrameAssembler a;
    annexb::AssembledFrame out;
    std::vector<uint8_t> empty;
    CHECK(!a.feed_nal(empty, 30, 1, &out));
}

TEST(assembler_flush_with_nothing_pending_returns_false) {
    annexb::FrameAssembler a;
    annexb::AssembledFrame out;
    CHECK(!a.flush(30, 1, &out));
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
    big_slice.push_back(0x80); // first_mb_in_slice = 0 (apre l'AU)
    big_slice.resize(2 + 4 * 1024 * 1024, 0xEE);
    CHECK(!a.feed_nal(big_slice, 30, 1, &out)); // apre l'AU: niente ancora da emettere

    std::vector<uint8_t> slice = make_slice(1, 0); // ogni chiamata apre (e quindi chiude la
                                                     // precedente) un nuovo access unit da 1 slice
    int64_t prev_pts = -1;
    for (int i = 0; i < 10000; i++) {
        const bool got = a.feed_nal(slice, 30, 1, &out);
        CHECK(got); // dalla seconda chiamata in poi (compresa quella che chiude big_slice) sempre vero
        if (got) {
            CHECK(out.pts_hns >= prev_pts);
            prev_pts = out.pts_hns;
        }
    }
    CHECK(a.flush(30, 1, &out)); // l'ultimo AU della sequenza resta in sospeso finche' non si fa flush
    // big_slice + i primi 9999 "slice" del loop chiusi via feed_nal, l'ultimo via flush: 1 + 10000.
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
