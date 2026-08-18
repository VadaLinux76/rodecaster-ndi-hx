#include "annexb.h"

namespace annexb {

// --- NalReader --------------------------------------------------------------------------------

size_t NalReader::find_start_code(const std::vector<uint8_t>& b, size_t from, size_t* len) {
    if (b.size() < 3) return npos;
    for (size_t i = from; i + 3 <= b.size(); i++) {
        if (b[i] == 0 && b[i + 1] == 0 && b[i + 2] == 1) {
            if (i > from && b[i - 1] == 0) { *len = 4; return i - 1; }
            *len = 3;
            return i;
        }
    }
    return npos;
}

// buf_.begin()+N vuole un difference_type (signed): questo helper accentra il cast da size_t,
// che altrimenti Clang segnala come sign-conversion su ogni singolo punto d'uso.
static std::vector<uint8_t>::const_iterator advance(std::vector<uint8_t>::const_iterator it, size_t n) {
    return it + static_cast<std::vector<uint8_t>::difference_type>(n);
}

bool NalReader::next_nal(std::vector<uint8_t>* out) {
    size_t len1 = 0;
    size_t sc1 = find_start_code(buf_, 0, &len1);
    if (sc1 == npos) return false;
    size_t len2 = 0;
    size_t sc2 = find_start_code(buf_, sc1 + len1, &len2);
    if (sc2 == npos) return false; // NAL non ancora completo: aspetta altri dati

    out->assign(advance(buf_.cbegin(), sc1 + len1), advance(buf_.cbegin(), sc2));
    buf_.erase(buf_.begin(), advance(buf_.cbegin(), sc2)); // lascia lo start code successivo in testa
    return true;
}

bool NalReader::flush(std::vector<uint8_t>* out) {
    size_t len1 = 0;
    size_t sc1 = find_start_code(buf_, 0, &len1);
    if (sc1 == npos) return false;
    out->assign(advance(buf_.cbegin(), sc1 + len1), buf_.cend());
    buf_.clear();
    return !out->empty();
}

void append_annexb(std::vector<uint8_t>* out, const std::vector<uint8_t>& nal) {
    static const uint8_t sc[4] = {0, 0, 0, 1};
    out->insert(out->end(), sc, sc + 4);
    out->insert(out->end(), nal.begin(), nal.end());
}

int64_t compute_hns(uint64_t frame_index, int fps_n, int fps_d) {
    return (int64_t)((double)frame_index * 10000000.0 * fps_d / fps_n);
}

// --- RbspBitReader ------------------------------------------------------------------------

bool RbspBitReader::is_emulation_prevention_byte() const {
    // 00 00 03 e' sempre uno stuffing byte inserito dall'encoder per evitare che "00 00 00",
    // "00 00 01", "00 00 02" o "00 00 03" appaiano nel NAL (verrebbero confusi con uno start
    // code). Il controllo va fatto sui byte RAW del NAL, non su quelli gia' de-escapati:
    // e' cosi' che un encoder decide dove inserirlo, quindi e' cosi' che va rimosso.
    return byte_pos_ >= 2 && data_[byte_pos_] == 0x03 &&
           data_[byte_pos_ - 1] == 0x00 && data_[byte_pos_ - 2] == 0x00;
}

bool RbspBitReader::read_bit(bool* out) {
    while (byte_pos_ < size_ && bit_pos_ == 0 && is_emulation_prevention_byte()) {
        byte_pos_++; // salta lo 0x03 di emulation prevention
    }
    if (byte_pos_ >= size_) return false;
    *out = ((data_[byte_pos_] >> (7 - bit_pos_)) & 1) != 0;
    bit_pos_++;
    if (bit_pos_ == 8) { bit_pos_ = 0; byte_pos_++; }
    return true;
}

bool RbspBitReader::read_bits(int n, uint32_t* out) {
    uint32_t v = 0;
    for (int i = 0; i < n; i++) {
        bool b;
        if (!read_bit(&b)) return false;
        v = (v << 1) | (b ? 1u : 0u);
    }
    *out = v;
    return true;
}

bool RbspBitReader::read_ue(uint32_t* out) {
    int zeros = 0;
    bool b;
    for (;;) {
        if (!read_bit(&b)) return false;
        if (b) break;
        zeros++;
        if (zeros > 31) return false; // valore assurdo per un ue(v) reale: NAL corrotto
    }
    if (zeros == 0) { *out = 0; return true; }
    uint32_t suffix;
    if (!read_bits(zeros, &suffix)) return false;
    *out = ((1u << zeros) - 1) + suffix;
    return true;
}

bool parse_first_mb_in_slice(const std::vector<uint8_t>& nal, uint32_t* out) {
    if (nal.size() < 2) return false; // serve almeno l'header NAL + un bit di slice_header
    RbspBitReader r(nal.data() + 1, nal.size() - 1); // salta l'header NAL (1 byte)
    return r.read_ue(out);
}

// --- FrameAssembler -----------------------------------------------------------------------

void FrameAssembler::open_au(int nal_type) {
    au_.clear();
    au_is_keyframe_ = (nal_type == 5) && have_params_;
    au_extradata_ = au_is_keyframe_ ? extradata_ : std::vector<uint8_t>{};
    au_pending_ = true;
}

bool FrameAssembler::close_au(int fps_n, int fps_d, AssembledFrame* out) {
    if (!au_pending_) return false;
    out->is_keyframe = au_is_keyframe_;
    out->pts_hns = out->dts_hns = compute_hns(frame_index_, fps_n, fps_d);
    out->data = au_;
    out->extra_data = au_extradata_;
    frame_index_++;
    au_pending_ = false;
    au_.clear();
    return true;
}

bool FrameAssembler::feed_nal(const std::vector<uint8_t>& nal, int fps_n, int fps_d, AssembledFrame* out) {
    if (nal.empty()) return false;
    const int nal_type = nal[0] & 0x1f;

    if (nal_type == 7 || nal_type == 8) {
        // SPS (7) / PPS (8): tenute "nude" (senza start code) e riassemblate in Annex-B.
        if (nal_type == 7) sps_nal_ = nal;
        if (nal_type == 8) pps_nal_ = nal;
        if (!sps_nal_.empty() && !pps_nal_.empty()) {
            extradata_.clear();
            append_annexb(&extradata_, sps_nal_);
            append_annexb(&extradata_, pps_nal_);
            have_params_ = true;
        }
        return false;
    }
    if (nal_type == 9) {
        // Access Unit Delimiter: se presente, e' il modo piu' economico e affidabile per
        // sapere che la prossima slice apre un nuovo access unit (non serve leggerne lo
        // slice header). Se lo stream non usa AUD questo ramo semplicemente non scatta mai e
        // si ricade sul parsing di first_mb_in_slice piu' sotto.
        aud_pending_ = true;
        return false;
    }
    if (nal_type == 6) {
        return false; // SEI: non serve nel payload NDI
    }
    if (nal_type == 1 || nal_type == 5) {
        uint32_t first_mb = 0;
        const bool parsed = parse_first_mb_in_slice(nal, &first_mb);
        // Se il parsing fallisce trattiamo prudentemente il NAL come inizio di un nuovo
        // access unit: meglio spezzare un frame in due che accumulare all'infinito dati
        // potenzialmente corrotti in un buffer che potrebbe non essere mai chiuso.
        const bool starts_new_au = aud_pending_ || !parsed || first_mb == 0;
        aud_pending_ = false;

        bool emitted = false;
        if (starts_new_au && au_pending_) {
            emitted = close_au(fps_n, fps_d, out);
        }
        if (starts_new_au || !au_pending_) {
            open_au(nal_type);
        }
        append_annexb(&au_, nal);
        return emitted;
    }
    // Altri tipi (filler, ecc.): ignorati.
    return false;
}

bool FrameAssembler::flush(int fps_n, int fps_d, AssembledFrame* out) {
    return close_au(fps_n, fps_d, out);
}

} // namespace annexb
