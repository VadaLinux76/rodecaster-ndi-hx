// Legge uno stream elementare H.264 Annex-B (NAL con start code 00 00 01 / 00 00 00 01) da
// stdin e lo ripubblica, sempre in Annex-B, dentro un pacchetto NDI in modalita' COMPRESSA
// (NDIlib_FourCC_video_type_ex_H264_*), cioe' lo stesso meccanismo di trasporto usato da
// NDI|HX. Richiede l'NDI Advanced SDK (necessario per i FourCC compressi e per la scrittura
// diretta di pacchetti gia' codificati).
//
// Formato dati: la documentazione ufficiale NDI e' esplicita — "NDI assumes that all H.264
// data is as specified in Annex B... and the data must include the start codes", sia per il
// campo dati che per extra_data (SPS/PPS). NON va convertito in AVCC/length-prefixed.
//
// Pensato per essere alimentato da ffmpeg che cattura la webcam (MJPEG/YUYV via V4L2) e la
// codifica in H.264 con l'encoder hardware del Pi4 (h264_v4l2m2m):
//
//   ffmpeg -f v4l2 -input_format mjpeg -video_size 1280x720 -framerate 30 -i /dev/video0
//       -pix_fmt nv12 -c:v h264_v4l2m2m -b:v 4M -g 60 -bf 0 -f h264 -
//   | ./ndi_hx_send "Pi4 Cam HX" 1280 720 30
//
// Uso: ndi_hx_send <nome_ndi> <larghezza> <altezza> <fps_num> [fps_den] [bandwidth]
//   bandwidth: "highest" (default) oppure "lowest".
//   Nota: "lowest_bandwidth" e' riservato, per specifica NDI, a un secondo stream di
//   anteprima a risoluzione FISSA 640px di larghezza — non e' "lo stesso video a bitrate
//   piu' basso". Per il flusso principale a risoluzione piena va sempre usato
//   "highest_bandwidth", qualunque sia la risoluzione reale inviata.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <csignal>
#include <cerrno>
#include <vector>
#include <unistd.h>
#include <Processing.NDI.Advanced.h>

static volatile sig_atomic_t g_stop = 0;
static void on_signal(int) { g_stop = 1; }

// --- Lettura incrementale da stdin con estrazione dei NAL Annex-B --------------------------

struct NalReader {
    std::vector<uint8_t> buf;
    static const size_t CHUNK = 65536;

    // Ritorna true se e' stato letto qualcosa (false = EOF).
    bool fill() {
        uint8_t tmp[CHUNK];
        ssize_t n = read(STDIN_FILENO, tmp, sizeof(tmp));
        if (n <= 0) {
            if (n < 0 && errno == EINTR) return true; // riprova
            return false;
        }
        buf.insert(buf.end(), tmp, tmp + n);
        return true;
    }

    // Trova l'offset del prossimo start code (00 00 01 o 00 00 00 01) a partire da `from`.
    // Ritorna la posizione di inizio del codice e la sua lunghezza (3 o 4), oppure npos.
    static const size_t npos = (size_t)-1;

    static size_t find_start_code(const std::vector<uint8_t>& b, size_t from, int* len) {
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

    // Estrae il prossimo NAL completo (payload, senza start code) se ne e' disponibile uno
    // interamente delimitato da due start code consecutivi. Consuma il buffer via erase.
    bool next_nal(std::vector<uint8_t>* out) {
        int len1 = 0;
        size_t sc1 = find_start_code(buf, 0, &len1);
        if (sc1 == npos) return false;
        int len2 = 0;
        size_t sc2 = find_start_code(buf, sc1 + len1, &len2);
        if (sc2 == npos) return false; // NAL non ancora completo: aspetta altri dati

        out->assign(buf.begin() + sc1 + len1, buf.begin() + sc2);
        buf.erase(buf.begin(), buf.begin() + sc2); // lascia lo start code successivo in testa
        return true;
    }
};

// Formato Annex-B: start code a 4 byte + NAL "nudo". Confermato dalla documentazione ufficiale
// NDI: "NDI assumes that all H.264 data is as specified in Annex B... and the data must
// include the start codes" - sia per i dati frame che per l'extra_data (SPS/PPS).
static void append_annexb(std::vector<uint8_t>* out, const std::vector<uint8_t>& nal) {
    static const uint8_t sc[4] = {0, 0, 0, 1};
    out->insert(out->end(), sc, sc + 4);
    out->insert(out->end(), nal.begin(), nal.end());
}

int main(int argc, char* argv[])
{
    if (argc < 4) {
        fprintf(stderr,
            "Uso: %s <nome_ndi> <larghezza> <altezza> <fps> [fps_den] [lowest|highest]\n",
            argv[0]);
        return 1;
    }

    const char* ndi_name = argv[1];
    const int width  = atoi(argv[2]);
    const int height = atoi(argv[3]);
    const int fps_n  = atoi(argv[4]);
    const int fps_d  = (argc > 5) ? atoi(argv[5]) : 1;
    const bool lowest_bw = (argc > 6) ? (strcmp(argv[6], "lowest") == 0) : false;

    if (width <= 0 || height <= 0 || fps_n <= 0) {
        fprintf(stderr, "Parametri non validi.\n");
        return 1;
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGPIPE, SIG_IGN);

    if (!NDIlib_initialize()) {
        fprintf(stderr, "NDIlib_initialize() fallita (CPU non supportata?).\n");
        return 1;
    }

    NDIlib_send_create_t create_desc;
    create_desc.p_ndi_name = ndi_name;
    create_desc.p_groups = NULL;
    create_desc.clock_video = true;
    create_desc.clock_audio = false;

    NDIlib_send_instance_t pNDI_send = NDIlib_send_create(&create_desc);
    if (!pNDI_send) {
        fprintf(stderr, "NDIlib_send_create() fallita.\n");
        NDIlib_destroy();
        return 1;
    }

    const NDIlib_FourCC_video_type_e fourcc = (NDIlib_FourCC_video_type_e)(
        lowest_bw ? NDIlib_FourCC_video_type_ex_H264_lowest_bandwidth
                  : NDIlib_FourCC_video_type_ex_H264_highest_bandwidth);

    fprintf(stderr, "NDI HX source '%s' avviata: %dx%d @ %d/%d fps (H264 %s bandwidth)\n",
            ndi_name, width, height, fps_n, fps_d, lowest_bw ? "lowest" : "highest");

    NalReader reader;
    std::vector<uint8_t> nal;
    std::vector<uint8_t> sps_nal, pps_nal;  // ultimi SPS/PPS "nudi" (senza start code)
    std::vector<uint8_t> annexb_extradata;  // SPS+PPS in Annex-B (start code + NAL, concatenati)
    std::vector<uint8_t> au;                // access unit corrente, formato Annex-B
    uint64_t frame_index = 0;
    uint64_t frames_sent = 0;
    bool have_params = false;

    auto emit_frame = [&](bool is_keyframe) {
        if (au.empty()) return;

        const size_t hdr_size = sizeof(NDIlib_compressed_packet_t);
        const size_t extra_size = is_keyframe ? annexb_extradata.size() : 0;
        const size_t total = hdr_size + au.size() + extra_size;

        std::vector<uint8_t> packet(total);
        NDIlib_compressed_packet_t* hdr = (NDIlib_compressed_packet_t*)packet.data();
        hdr->version = NDIlib_compressed_packet_version_0;
        hdr->fourCC = NDIlib_compressed_FourCC_type_H264;
        int64_t hns = (int64_t)((double)frame_index * 10000000.0 * fps_d / fps_n);
        hdr->pts = hns;
        hdr->dts = hns;
        hdr->reserved = 0;
        hdr->flags = is_keyframe ? NDIlib_compressed_packet_flags_keyframe
                                  : NDIlib_compressed_packet_flags_none;
        hdr->data_size = (uint32_t)au.size();
        hdr->extra_data_size = (uint32_t)extra_size;

        memcpy(packet.data() + hdr_size, au.data(), au.size());
        if (extra_size) memcpy(packet.data() + hdr_size + au.size(), annexb_extradata.data(), extra_size);

        NDIlib_video_frame_v2_t frame;
        frame.xres = width;
        frame.yres = height;
        frame.FourCC = fourcc;
        frame.frame_rate_N = fps_n;
        frame.frame_rate_D = fps_d;
        frame.picture_aspect_ratio = (float)width / (float)height; // 0 non va bene per stream compressi
        frame.frame_format_type = NDIlib_frame_format_type_progressive;
        frame.timecode = hns; // deve essere il PTS reale, non il sentinel "synthesize"
        frame.p_data = packet.data();
        frame.data_size_in_bytes = (int)total;
        frame.p_metadata = NULL;
        frame.timestamp = 0;

        if (is_keyframe && frames_sent == 0) {
            // Debug una tantum: SPS/PPS "nudi" e il pacchetto NDI esatto del primo keyframe,
            // per poterli ispezionare con ffprobe/hexdump fuori banda.
            FILE* f1 = fopen("/tmp/hx_debug_sps_annexb.h264", "wb");
            if (f1) {
                static const uint8_t sc[4] = {0, 0, 0, 1};
                fwrite(sc, 1, 4, f1); fwrite(sps_nal.data(), 1, sps_nal.size(), f1);
                fwrite(sc, 1, 4, f1); fwrite(pps_nal.data(), 1, pps_nal.size(), f1);
                fclose(f1);
            }
            FILE* f2 = fopen("/tmp/hx_debug_extradata.bin", "wb");
            if (f2) { fwrite(annexb_extradata.data(), 1, annexb_extradata.size(), f2); fclose(f2); }
            FILE* f3 = fopen("/tmp/hx_debug_ndi_packet.bin", "wb");
            if (f3) { fwrite(packet.data(), 1, packet.size(), f3); fclose(f3); }
            fprintf(stderr, "[ndi_hx_send][debug] sps=%zuB pps=%zuB extradata=%zuB au=%zuB packet_tot=%zuB "
                             "-> /tmp/hx_debug_*.bin\n",
                    sps_nal.size(), pps_nal.size(), annexb_extradata.size(), au.size(), packet.size());
        }

        NDIlib_send_send_video_scatter(pNDI_send, &frame, NULL);
        frames_sent++;
        frame_index++;
        if (frames_sent % (uint64_t)(fps_n * 10 / fps_d) == 0) { // ~ogni 10s
            fprintf(stderr, "[ndi_hx_send] %llu frame inviati, connessioni attive=%d\n",
                    (unsigned long long)frames_sent, NDIlib_send_get_no_connections(pNDI_send, 0));
        }
        au.clear();
    };

    while (!g_stop) {
        while (reader.next_nal(&nal)) {
            if (nal.empty()) continue;
            const int nal_type = nal[0] & 0x1f;

            if (nal_type == 7 || nal_type == 8) {
                // SPS (7) / PPS (8): tenute "nude" (senza start code) e riassemblate in Annex-B.
                if (nal_type == 7) sps_nal = nal;
                if (nal_type == 8) pps_nal = nal;
                if (!sps_nal.empty() && !pps_nal.empty()) {
                    annexb_extradata.clear();
                    append_annexb(&annexb_extradata, sps_nal);
                    append_annexb(&annexb_extradata, pps_nal);
                    have_params = true;
                }
                continue;
            }
            if (nal_type == 9 || nal_type == 6) {
                continue; // AUD / SEI: non servono nel payload NDI
            }
            if (nal_type == 1 || nal_type == 5) {
                // Slice (non-IDR / IDR): un frame per slice, dato che l'encoder non fa slicing.
                append_annexb(&au, nal);
                emit_frame(/*is_keyframe=*/nal_type == 5 && have_params);
                continue;
            }
            // Altri tipi (filler, ecc.): ignorati.
        }
        if (!reader.fill()) break; // EOF su stdin (ffmpeg terminato)
    }

    fprintf(stderr, "Stream terminato. Inviati %llu frame.\n", (unsigned long long)frames_sent);

    NDIlib_send_destroy(pNDI_send);
    NDIlib_destroy();
    return 0;
}
