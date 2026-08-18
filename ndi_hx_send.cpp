// Legge uno stream elementare H.264 Annex-B (NAL con start code 00 00 01 / 00 00 00 01) da
// stdin e lo ripubblica, sempre in Annex-B, dentro un pacchetto NDI in modalita' COMPRESSA
// (NDIlib_FourCC_video_type_ex_H264_*), cioe' lo stesso meccanismo di trasporto usato da
// NDI|HX. Richiede l'NDI Advanced SDK (necessario per i FourCC compressi e per la scrittura
// diretta di pacchetti gia' codificati).
//
// La logica di parsing Annex-B/ricostruzione delle access unit vive in annexb.h/.cpp, che non
// dipende dall'SDK NDI e ha una sua suite di test indipendente (vedi tests/) — questo file resta
// un wrapper sottile: legge stdin, chiama nell'ordine annexb::NalReader/FrameAssembler, e
// traduce ogni annexb::AssembledFrame in un pacchetto NDIlib_compressed_packet_t
// (code review: Codex, fase 2 - "separare dal codice NDI").
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
//
// Limite noto: un NAL di tipo slice (1 o 5) e' trattato come un frame completo — nessun
// supporto per access unit composte da piu' slice. Va bene con l'encoder hardware del Pi4
// (h264_v4l2m2m, che non fa slicing) ma non e' un'assunzione valida per qualunque encoder.
//
// Variabile d'ambiente NDI_HX_DEBUG: se definita (a qualunque valore), scrive in
// /tmp/hx_debug_* i byte grezzi del primo keyframe per ispezione con ffprobe/hexdump.
// Disattivato di default: in produzione scrivere sempre in /tmp e' un effetto collaterale non
// richiesto su una macchina potenzialmente condivisa (code review: Codex).

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <csignal>
#include <cerrno>
#include <climits>
#include <vector>
#include <unistd.h>
#include <Processing.NDI.Advanced.h>
#include "annexb.h"

static volatile sig_atomic_t g_stop = 0;
static void on_signal(int) { g_stop = 1; }

int main(int argc, char* argv[])
{
    // Fix: il controllo era "argc < 4" ma piu' sotto si legge argv[4] (fps_n). Con argc==4
    // argv[4] e' argv[argc], che lo standard C garantisce essere NULL: atoi(NULL) crasha.
    // Servono almeno 5 elementi (argv[0]=binario + 4 argomenti obbligatori).
    // (code review: Codex)
    if (argc < 5) {
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

    // Fix: fps_d non era validato (code review: Codex). Con fps_d<=0 il calcolo di pts/dts
    // degenera silenziosamente (es. sempre 0, o negativo), producendo un flusso NDI invalido
    // senza nessun errore visibile.
    if (width <= 0 || height <= 0 || fps_n <= 0 || fps_d <= 0) {
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

    // Zero-init (code review: Codex): protegge da campi non assegnati con valore
    // indeterminato, incluso il caso di future versioni dell'SDK che aggiungano campi che
    // questo codice non conosce ancora.
    NDIlib_send_create_t create_desc{};
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

    // Attiva lo scarico di file di debug in /tmp (vedi sotto) solo se richiesto esplicitamente:
    // prima veniva sempre scritto al primo keyframe, il che in produzione crea un effetto
    // collaterale non richiesto, puo' sovrascrivere diagnostica precedente ed espone porzioni
    // del flusso video ad altri processi locali su una macchina condivisa (code review: Codex).
    const bool debug_dump = getenv("NDI_HX_DEBUG") != nullptr;

    annexb::NalReader reader;
    annexb::FrameAssembler assembler;
    std::vector<uint8_t> nal;
    uint64_t frames_sent = 0;

    // Traduce un annexb::AssembledFrame in un pacchetto NDIlib_compressed_packet_t e lo invia.
    // E' l'unico punto in cui questo file tocca l'SDK NDI per il payload video.
    auto send_frame = [&](const annexb::AssembledFrame& af) {
        const size_t hdr_size = sizeof(NDIlib_compressed_packet_t);
        const size_t total = hdr_size + af.data.size() + af.extra_data.size();

        // Guardia overflow (code review: Codex): frame.data_size_in_bytes e' un int con segno,
        // quindi total non puo' eccedere INT_MAX senza troncare/wrap-around nel cast piu' sotto.
        // In pratica non capita con un encoder H.264 sano, ma un NAL corrotto o abnorme non deve
        // poter produrre un pacchetto NDI silenziosamente malformato.
        if (total > (size_t)INT_MAX) {
            fprintf(stderr, "[ndi_hx_send] frame scartato: %zu byte eccede INT_MAX.\n", total);
            return;
        }

        std::vector<uint8_t> packet(total);
        NDIlib_compressed_packet_t* hdr = (NDIlib_compressed_packet_t*)packet.data();
        hdr->version = NDIlib_compressed_packet_version_0;
        hdr->fourCC = NDIlib_compressed_FourCC_type_H264;
        hdr->pts = af.pts_hns;
        hdr->dts = af.dts_hns;
        hdr->reserved = 0;
        hdr->flags = af.is_keyframe ? NDIlib_compressed_packet_flags_keyframe
                                     : NDIlib_compressed_packet_flags_none;
        hdr->data_size = (uint32_t)af.data.size();
        hdr->extra_data_size = (uint32_t)af.extra_data.size();

        memcpy(packet.data() + hdr_size, af.data.data(), af.data.size());
        if (!af.extra_data.empty())
            memcpy(packet.data() + hdr_size + af.data.size(), af.extra_data.data(), af.extra_data.size());

        NDIlib_video_frame_v2_t frame{}; // zero-init, vedi nota sopra (code review: Codex)
        frame.xres = width;
        frame.yres = height;
        frame.FourCC = fourcc;
        frame.frame_rate_N = fps_n;
        frame.frame_rate_D = fps_d;
        frame.picture_aspect_ratio = (float)width / (float)height; // 0 non va bene per stream compressi
        frame.frame_format_type = NDIlib_frame_format_type_progressive;
        frame.timecode = af.pts_hns; // deve essere il PTS reale, non il sentinel "synthesize"
        frame.p_data = packet.data();
        frame.data_size_in_bytes = (int)total;
        frame.p_metadata = NULL;
        frame.timestamp = 0;

        if (debug_dump && af.is_keyframe && frames_sent == 0) {
            // Debug una tantum (solo se NDI_HX_DEBUG e' impostata, vedi sopra): il pacchetto
            // NDI esatto del primo keyframe, per ispezionarlo con ffprobe/hexdump fuori banda.
            FILE* f3 = fopen("/tmp/hx_debug_ndi_packet.bin", "wb");
            if (f3) { fwrite(packet.data(), 1, packet.size(), f3); fclose(f3); }
            fprintf(stderr, "[ndi_hx_send][debug] data=%zuB extra=%zuB packet_tot=%zuB "
                             "-> /tmp/hx_debug_ndi_packet.bin\n",
                    af.data.size(), af.extra_data.size(), packet.size());
        }

        NDIlib_send_send_video_scatter(pNDI_send, &frame, NULL);
        frames_sent++;
        if (frames_sent % (uint64_t)(fps_n * 10 / fps_d) == 0) { // ~ogni 10s
            fprintf(stderr, "[ndi_hx_send] %llu frame inviati, connessioni attive=%d\n",
                    (unsigned long long)frames_sent, NDIlib_send_get_no_connections(pNDI_send, 0));
        }
    };

    annexb::AssembledFrame af;
    while (!g_stop) {
        while (reader.next_nal(&nal)) {
            if (assembler.feed_nal(nal, fps_n, fps_d, &af)) send_frame(af);
        }
        uint8_t tmp[65536];
        ssize_t n = read(STDIN_FILENO, tmp, sizeof(tmp));
        if (n > 0) {
            reader.feed(tmp, (size_t)n);
        } else if (n < 0 && errno == EINTR) {
            continue; // riprova
        } else {
            break; // EOF (n==0) o errore di lettura su stdin (ffmpeg terminato)
        }
    }
    if (reader.flush(&nal) && assembler.feed_nal(nal, fps_n, fps_d, &af)) send_frame(af);

    fprintf(stderr, "Stream terminato. Inviati %llu frame.\n", (unsigned long long)frames_sent);

    NDIlib_send_destroy(pNDI_send);
    NDIlib_destroy();
    return 0;
}
