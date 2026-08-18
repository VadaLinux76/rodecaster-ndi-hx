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
// Limite noto: un NAL di tipo slice (1 o 5) e' trattato come una access unit completa -- nessun
// supporto per frame composti da piu' slice. Va bene con l'encoder hardware del Pi4
// (h264_v4l2m2m, che non fa slicing) ma non e' un'assunzione valida per qualunque encoder H.264.

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

// Un frame Annex-B pronto per essere impacchettato in un pacchetto NDI compresso.
struct AssembledFrame {
    bool is_keyframe = false;
    int64_t pts_hns = 0;
    int64_t dts_hns = 0;
    std::vector<uint8_t> data;       // Annex-B: la slice di questo frame
    std::vector<uint8_t> extra_data; // Annex-B: SPS+PPS correnti, presente solo sui keyframe
};

// Accumula NAL "nudi" (senza start code, come restituiti da NalReader) e ricostruisce le
// access unit. Un NAL SPS (7) o PPS (8) aggiorna i parametri correnti (extra_data viene
// ricostruito ogni volta che sia SPS che PPS sono noti, quindi anche su SPS/PPS ripetuti). Un
// NAL slice (tipo 1 o 5) chiude e restituisce un frame. Un IDR (tipo 5) e' marcato keyframe
// solo se SPS+PPS sono gia' noti a quel punto (altrimenti verrebbe generato un keyframe senza
// i parametri necessari a decodificarlo). NAL di altro tipo (AUD, SEI, filler...) sono ignorati.
class FrameAssembler {
public:
    // Se il NAL fornito completa un frame, lo scrive in *out e ritorna true.
    bool feed_nal(const std::vector<uint8_t>& nal, int fps_n, int fps_d, AssembledFrame* out);

    uint64_t frames_emitted() const { return frame_index_; }

private:
    std::vector<uint8_t> sps_nal_, pps_nal_;
    std::vector<uint8_t> extradata_; // SPS+PPS in Annex-B, ricostruito ad ogni coppia completa
    std::vector<uint8_t> au_;
    uint64_t frame_index_ = 0;
    bool have_params_ = false;
};

} // namespace annexb
