#!/bin/bash
# Cattura una webcam V4L2 (MJPEG), la codifica in H.264 con l'encoder hardware del Pi4
# (bcm2835-codec / h264_v4l2m2m) e la pubblica come sorgente NDI HX (compressa).
#
# I keyframe sono forzati molto ravvicinati (default: ogni 2 frame) invece che ogni paio
# di secondi. Motivo: l'NDI Advanced SDK segnala a runtime che per essere "NDI|HX compliant"
# un I-frame deve arrivare entro 100ms da quando un receiver si connette/lo richiede. ffmpeg
# non permette di pilotare l'encoder in modo reattivo su richiesta, quindi l'unica strada e'
# tenere il gruppo di immagini cosi' corto che ce n'e' sempre uno "fresco" (< 100ms) pronto.
# Costo: bitrate piu' alto a parita' di qualita' (gli I-frame pesano piu' dei P-frame).
#
# Uso: ./capture_cam_hx.sh <device> <larghezza> <altezza> <fps> <nome_ndi> [bitrate] [gop_frame]
# Esempio:
#   ./capture_cam_hx.sh /dev/video0 1920 1080 30 "Pi4 Cam HX" 8M 2

set -euo pipefail
cd "$(dirname "$0")"

DEVICE="${1:?device, es. /dev/video0}"
WIDTH="${2:?larghezza, es. 1920}"
HEIGHT="${3:?altezza, es. 1080}"
FPS="${4:?fps, es. 30}"
NDI_NAME="${5:?nome sorgente NDI, es. \"Pi4 Cam HX\"}"
BITRATE="${6:-8M}"
GOP="${7:-2}"
H264_PROFILE="${8:-77}" # AVCodecContext.profile: 66=Baseline 77=Main 100=High (default HW: 100/High)
H264_LEVEL="${9:-31}"   # AVCodecContext.level *10: 31=Level 3.1, 40=Level 4.0

# L'encoder hardware bcm2835-codec di default usa H.264 High Profile / Level 4, che receiver
# NDI|HX certificati (es. RODECaster) rifiutano ("unsupported resolution 0x0" e' il sintomo:
# non riescono a validare/decodificare lo stream). Nota: /dev/videoN per un codec V4L2 mem2mem
# e' stateless, ogni open() crea un'istanza indipendente con i default hardware, quindi
# impostare il profilo con v4l2-ctl PRIMA non ha alcun effetto sull'istanza aperta da ffmpeg:
# va passato a ffmpeg stesso (che lo applica alla propria istanza) come valore numerico
# (i nomi tipo "main" non sono accettati da questo encoder).

exec ffmpeg -hide_banner -loglevel warning \
    -f v4l2 -input_format mjpeg -video_size "${WIDTH}x${HEIGHT}" -framerate "$FPS" \
    -i "$DEVICE" \
    -pix_fmt nv12 -c:v h264_v4l2m2m -b:v "$BITRATE" -g "$GOP" -bf 0 \
    -profile:v "$H264_PROFILE" -level:v "$H264_LEVEL" \
    -num_capture_buffers 16 \
    -force_key_frames "expr:eq(mod(n,${GOP}),0)" \
    -f h264 - \
| ./ndi_hx_send "$NDI_NAME" "$WIDTH" "$HEIGHT" "$FPS" 1 highest
