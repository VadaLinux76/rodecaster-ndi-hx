#include "annexb.h"

namespace annexb {

size_t NalReader::find_start_code(const std::vector<uint8_t>& b, size_t from, int* len) {
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

bool NalReader::next_nal(std::vector<uint8_t>* out) {
    int len1 = 0;
    size_t sc1 = find_start_code(buf_, 0, &len1);
    if (sc1 == npos) return false;
    int len2 = 0;
    size_t sc2 = find_start_code(buf_, sc1 + len1, &len2);
    if (sc2 == npos) return false; // NAL non ancora completo: aspetta altri dati

    out->assign(buf_.begin() + sc1 + len1, buf_.begin() + sc2);
    buf_.erase(buf_.begin(), buf_.begin() + sc2); // lascia lo start code successivo in testa
    return true;
}

bool NalReader::flush(std::vector<uint8_t>* out) {
    int len1 = 0;
    size_t sc1 = find_start_code(buf_, 0, &len1);
    if (sc1 == npos) return false;
    out->assign(buf_.begin() + sc1 + len1, buf_.end());
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
    if (nal_type == 9 || nal_type == 6) {
        return false; // AUD / SEI: non servono nel payload NDI
    }
    if (nal_type == 1 || nal_type == 5) {
        // Slice (non-IDR / IDR): un frame per slice, dato che l'encoder non fa slicing (vedi
        // limite noto in annexb.h). Un IDR e' un vero keyframe solo se SPS+PPS sono gia' noti.
        const bool is_keyframe = (nal_type == 5) && have_params_;
        au_.clear();
        append_annexb(&au_, nal);

        out->is_keyframe = is_keyframe;
        out->pts_hns = out->dts_hns = compute_hns(frame_index_, fps_n, fps_d);
        out->data = au_;
        out->extra_data = is_keyframe ? extradata_ : std::vector<uint8_t>{};

        frame_index_++;
        return true;
    }
    // Altri tipi (filler, ecc.): ignorati.
    return false;
}

} // namespace annexb
