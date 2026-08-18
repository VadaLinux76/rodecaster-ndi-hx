#pragma once
// Modulo puro C++ (nessuna dipendenza dall'SDK NDI, nessun I/O reale) per il parsing di uno
// stream elementare H.264 Annex-B e la ricostruzione delle access unit da pubblicare come
// NDI|HX. Estratto da ndi_hx_send.cpp per essere compilabile e testabile senza il proprietario
// NDI Advanced SDK, che in CI non e' scaricabile automaticamente (code review: Codex, fase 2 -
// "separare dal codice NDI: parser Annex-B, ricostruzione access unit, gestione SPS/PPS,
// calcolo PTS/DTS, assemblaggio logico del pacchetto").
//
// Formato dati: la documentazione ufficiale NDI e' esplicita — "NDI assumes that all H.264
// data is as specified in Annex B... and the data must include the start codes", sia per il
// campo dati che per extra_data (SPS/PPS). Questo modulo lavora sempre in Annex-B.
//
// Access unit multi-slice (code review: Codex, fase 3 - "gestire access unit multi-slice"):
// FrameAssembler riconosce l'inizio di un nuovo access unit tramite un Access Unit Delimiter
// (NAL tipo 9), quando presente, oppure — quando assente, come nell'output dell'encoder
// hardware del Pi4 — leggendo first_mb_in_slice dall'inizio dello slice header di ogni NAL
// slice (7.4.1.2.4 della spec H.264: un nuovo access unit inizia quando first_mb_in_slice
// torna a 0). Questo introduce una latenza di un access unit: un frame viene restituito solo
// quando arriva la prima slice del frame successivo, o a fine flusso via flush().

#include <cstddef>
#include <cstdint>
#include <vector>

namespace annexb {

// --- Estrazione dei NAL da un flusso Annex-B -------------------------------------------------
//
// Non fa I/O: il chiamante alimenta i byte via feed() (equivalente a un read() riuscito, in
// qualunque dimensione di chunk) e poi chiama next_nal() finche' ritorna frame disponibili.
class NalReader {
public:
    static constexpr size_t npos = (size_t)-1;

    void feed(const uint8_t* data, size_t len) { buf_.insert(buf_.end(), data, data + len); }
    void feed(const std::vector<uint8_t>& data) { feed(data.data(), data.size()); }

    // Estrae il prossimo NAL completo (payload, senza start code) se ne e' disponibile uno
    // interamente delimitato da due start code consecutivi (3 o 4 byte: 00 00 01 / 00 00 00 01).
    // Consuma il buffer via erase.
    bool next_nal(std::vector<uint8_t>* out);

    // Da chiamare a EOF: un NAL delimitato da un solo start code (senza uno successivo, perche'
    // i dati sono finiti) non puo' mai essere restituito da next_nal(). flush() lo recupera
    // comunque, prendendo tutto cio' che segue l'ultimo start code fino alla fine del buffer.
    bool flush(std::vector<uint8_t>* out);

    bool empty() const { return buf_.empty(); }
    size_t buffered_bytes() const { return buf_.size(); }

private:
    std::vector<uint8_t> buf_;
    static size_t find_start_code(const std::vector<uint8_t>& b, size_t from, size_t* len);
};

// Annex-B: start code a 4 byte + NAL "nudo".
void append_annexb(std::vector<uint8_t>* out, const std::vector<uint8_t>& nal);

// PTS/DTS in unita' di 100ns (HNS, lo stesso schema usato da NDI), dato l'indice progressivo
// del frame e il frame rate fps_n/fps_d. fps_n deve essere > 0.
int64_t compute_hns(uint64_t frame_index, int fps_n, int fps_d);

// --- Parsing minimale dello slice header (solo first_mb_in_slice) ---------------------------
//
// Legge bit dal payload RBSP di un NAL rimuovendo al volo i byte di emulation-prevention
// (0x03 dopo due 0x00 raw consecutivi, H.264 Annex B / 7.3.1). Non e' un parser RBSP completo:
// serve solo a leggere i primi campi dello slice header.
class RbspBitReader {
public:
    RbspBitReader(const uint8_t* data, size_t size) : data_(data), size_(size) {}

    // Legge n bit (1 <= n <= 32) come intero big-endian. False se i dati finiscono prima.
    bool read_bits(int n, uint32_t* out);

    // Legge un valore Exp-Golomb senza segno ue(v): conta gli zero iniziali, poi legge
    // altrettanti bit + 1 come mantissa (Tabella 9-2 della spec H.264).
    bool read_ue(uint32_t* out);

private:
    bool read_bit(bool* out);
    bool is_emulation_prevention_byte() const;

    const uint8_t* data_;
    size_t size_;
    size_t byte_pos_ = 0;
    int bit_pos_ = 0;
};

// Estrae first_mb_in_slice (il primissimo campo di slice_header(), H.264 7.3.3) da un NAL di
// tipo 1 o 5. Ritorna false se il NAL e' troppo corto/corrotto per contenere un valore
// leggibile -- in tal caso il chiamante deve trattare il NAL con prudenza (FrameAssembler lo
// considera come inizio di un nuovo access unit, per non rischiare di accumulare dati corrotti
// all'infinito in un buffer che non verrebbe mai emesso).
bool parse_first_mb_in_slice(const std::vector<uint8_t>& nal, uint32_t* out);

// Un frame Annex-B pronto per essere impacchettato in un pacchetto NDI compresso. Puo'
// contenere piu' di un NAL slice (Annex-B concatenati) se l'access unit era multi-slice.
struct AssembledFrame {
    bool is_keyframe = false;
    int64_t pts_hns = 0;
    int64_t dts_hns = 0;
    std::vector<uint8_t> data;       // Annex-B: tutte le slice di questo access unit
    std::vector<uint8_t> extra_data; // Annex-B: SPS+PPS correnti, presente solo sui keyframe
};

// Accumula NAL "nudi" (senza start code, come restituiti da NalReader) e ricostruisce le
// access unit, incluse quelle composte da piu' slice. Un NAL SPS (7) o PPS (8) aggiorna i
// parametri correnti (extra_data viene ricostruito ogni volta che sia SPS che PPS sono noti,
// quindi anche su SPS/PPS ripetuti). Un Access Unit Delimiter (9), quando presente, forza
// l'inizio di un nuovo access unit alla prossima slice; in sua assenza si usa
// first_mb_in_slice. Un IDR (tipo 5) e' marcato keyframe solo se SPS+PPS sono gia' noti quando
// l'access unit viene aperto.
//
// A causa della latenza di un access unit (vedi sopra), l'ultimo frame del flusso non viene
// mai restituito da feed_nal(): va recuperato esplicitamente con flush() a fine stream.
class FrameAssembler {
public:
    // Se il NAL fornito completa un access unit precedente, lo scrive in *out e ritorna true.
    bool feed_nal(const std::vector<uint8_t>& nal, int fps_n, int fps_d, AssembledFrame* out);

    // Da chiamare a fine stream: se c'e' un access unit ancora in accumulo (non ancora chiuso
    // perche' non e' arrivata la prima slice di quello successivo), lo restituisce.
    bool flush(int fps_n, int fps_d, AssembledFrame* out);

    uint64_t frames_emitted() const { return frame_index_; }

private:
    std::vector<uint8_t> sps_nal_, pps_nal_;
    std::vector<uint8_t> extradata_; // SPS+PPS in Annex-B, ricostruito ad ogni coppia completa

    std::vector<uint8_t> au_;          // access unit in accumulo (puo' contenere piu' slice)
    bool au_pending_ = false;          // au_ contiene almeno una slice non ancora emessa
    bool au_is_keyframe_ = false;      // vero se la prima slice dell'AU era un IDR con SPS/PPS noti
    std::vector<uint8_t> au_extradata_; // extradata "congelato" al momento dell'apertura dell'AU
    bool aud_pending_ = false;         // e' appena arrivato un AUD: la prossima slice apre un nuovo AU

    uint64_t frame_index_ = 0;
    bool have_params_ = false;

    void open_au(int nal_type);
    bool close_au(int fps_n, int fps_d, AssembledFrame* out);
};

} // namespace annexb
