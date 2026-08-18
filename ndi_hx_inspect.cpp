// Si collega a una sorgente NDI (per nome) in modalita' COMPRESSA (nessun decode)
// e stampa la struttura reale dei pacchetti H264/HEVC/SpeedHQ ricevuti: utile per
// confrontare un vero sender HX (es. RODE Capture su iPhone) con il nostro.
#include <cstdio>
#include <cstring>
#include <chrono>
#include <Processing.NDI.Advanced.h>

static void hexdump(const uint8_t* p, int n, const char* label)
{
    fprintf(stderr, "  %s (%d byte): ", label, n);
    int show = n < 256 ? n : 256;
    for (int i = 0; i < show; i++) fprintf(stderr, "%02x ", p[i]);
    if (n > show) fprintf(stderr, "...");
    fprintf(stderr, "\n");
}

int main(int argc, char* argv[])
{
    const char* name_filter = (argc > 1) ? argv[1] : "";

    if (!NDIlib_initialize()) return 1;
    NDIlib_find_instance_t pFind = NDIlib_find_create_v2();
    if (!pFind) return 1;

    const NDIlib_source_t* p_sources = nullptr;
    uint32_t no_sources = 0;
    const NDIlib_source_t* chosen = nullptr;

    for (int tries = 0; tries < 15 && !chosen; tries++) {
        NDIlib_find_wait_for_sources(pFind, 1000);
        p_sources = NDIlib_find_get_current_sources(pFind, &no_sources);
        fprintf(stderr, "Sorgenti viste (%u):\n", no_sources);
        for (uint32_t i = 0; i < no_sources; i++) {
            fprintf(stderr, "  - %s\n", p_sources[i].p_ndi_name);
            if (!chosen && strstr(p_sources[i].p_ndi_name, name_filter))
                chosen = &p_sources[i];
        }
    }
    if (!chosen) { fprintf(stderr, "Sorgente '%s' non trovata.\n", name_filter); return 1; }
    fprintf(stderr, "Connessione (COMPRESSED) a: %s\n", chosen->p_ndi_name);

    NDIlib_recv_create_v3_t recv_desc;
    recv_desc.source_to_connect_to = *chosen;
    recv_desc.color_format = (NDIlib_recv_color_format_e)NDIlib_recv_color_format_ex_compressed_v3;
    recv_desc.bandwidth = NDIlib_recv_bandwidth_highest;
    recv_desc.allow_video_fields = false;
    recv_desc.p_ndi_recv_name = "HX Inspector";

    NDIlib_recv_instance_t pRecv = NDIlib_recv_create_v3(&recv_desc);
    if (!pRecv) { fprintf(stderr, "recv_create fallita\n"); return 1; }

    using namespace std::chrono;
    int shown = 0;
    int polls = 0;
    for (const auto start = high_resolution_clock::now(); high_resolution_clock::now() - start < seconds(12);) {
        NDIlib_video_frame_v2_t vf;
        NDIlib_audio_frame_v2_t af;
        NDIlib_metadata_frame_t mf;
        auto ft = NDIlib_recv_capture_v2(pRecv, &vf, &af, &mf, 1000);
        polls++;
        fprintf(stderr, "[poll %d] frame_type=%d\n", polls, (int)ft);
        if (ft == NDIlib_frame_type_video) {
            fprintf(stderr, "\n=== video frame: xres=%d yres=%d FourCC=0x%08x aspect=%.3f fmt_type=%d ===\n",
                    vf.xres, vf.yres, (unsigned)vf.FourCC, vf.picture_aspect_ratio, (int)vf.frame_format_type);

            bool is_compressed = (vf.FourCC == (NDIlib_FourCC_video_type_e)NDIlib_compressed_FourCC_type_H264)
                               || (vf.FourCC == (NDIlib_FourCC_video_type_e)NDIlib_compressed_FourCC_type_HEVC)
                               || ((unsigned)vf.FourCC == (unsigned)NDIlib_FourCC_video_type_ex_H264_lowest_bandwidth)
                               || ((unsigned)vf.FourCC == (unsigned)NDIlib_FourCC_video_type_ex_HEVC_lowest_bandwidth);

            if (is_compressed && vf.p_data) {
                const NDIlib_compressed_packet_t* hdr = (const NDIlib_compressed_packet_t*)vf.p_data;
                fprintf(stderr, "  packet.version=%d (sizeof=%zu) fourCC=0x%08x flags=%u data_size=%u extra_data_size=%u pts=%lld dts=%lld\n",
                        hdr->version, sizeof(NDIlib_compressed_packet_t), (unsigned)hdr->fourCC, hdr->flags,
                        hdr->data_size, hdr->extra_data_size, (long long)hdr->pts, (long long)hdr->dts);
                const uint8_t* p_frame_data = (const uint8_t*)hdr + hdr->version;
                const uint8_t* p_extra = p_frame_data + hdr->data_size;
                hexdump(p_frame_data, hdr->data_size, "data (inizio)");
                if (hdr->extra_data_size) hexdump(p_extra, hdr->extra_data_size, "extra_data (inizio)");
            } else {
                fprintf(stderr, "  (non compresso o p_data nullo -- FourCC non H264/HEVC)\n");
            }
            shown++;
            NDIlib_recv_free_video_v2(pRecv, &vf);
        } else if (ft == NDIlib_frame_type_audio) {
            NDIlib_recv_free_audio_v2(pRecv, &af);
        } else if (ft == NDIlib_frame_type_metadata) {
            fprintf(stderr, "  [metadata] %s\n", mf.p_data ? mf.p_data : "(null)");
            NDIlib_recv_free_metadata(pRecv, &mf);
        }
    }

    NDIlib_recv_destroy(pRecv);
    NDIlib_find_destroy(pFind);
    NDIlib_destroy();
    return 0;
}
