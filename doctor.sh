#!/bin/bash
# Autodiagnosi: verifica i prerequisiti di questo progetto sul sistema corrente e prova a
# indicare la causa più probabile quando qualcosa manca (code review: Codex - "comando di
# autodiagnosi"). Solo controlli in lettura: non tocca mai la webcam in streaming (niente
# S_FMT/VIDIOC_STREAMON), quindi è sicuro da lanciare anche mentre il servizio è già attivo o
# mentre altri processi (es. un altro programma che usa la stessa webcam) la stanno usando.
#
# Uso: ./doctor.sh [device webcam, es. /dev/video0]

set -uo pipefail # niente -e: si vuole continuare anche se un singolo controllo fallisce
cd "$(dirname "$0")"

PASS=0
FAIL=0
WARN=0

ok()      { printf "  \033[32m✓\033[0m %s\n" "$1"; PASS=$((PASS + 1)); }
bad()     { printf "  \033[31m✗\033[0m %s\n" "$1"; FAIL=$((FAIL + 1)); }
warn()    { printf "  \033[33m!\033[0m %s\n" "$1"; WARN=$((WARN + 1)); }
section() { printf "\n\033[1m%s\033[0m\n" "$1"; }

DEVICE="${1:-}"

section "Strumenti di base"

if command -v ffmpeg >/dev/null 2>&1; then
    ok "ffmpeg trovato: $(ffmpeg -version 2>/dev/null | head -1)"
else
    bad "ffmpeg non trovato nel PATH"
fi

if command -v v4l2-ctl >/dev/null 2>&1; then
    ok "v4l2-ctl trovato (pacchetto v4l-utils)"
else
    bad "v4l2-ctl non trovato -- installa il pacchetto v4l-utils"
fi

section "Encoder H.264 hardware (h264_v4l2m2m)"

# Nota: si cattura sempre l'output in una variabile prima di grep -q, invece di usare
# `comando | grep -q ...` direttamente. Con `set -o pipefail` e un match trovato subito, grep -q
# chiude la pipe in anticipo, il produttore a monte muore di SIGPIPE, e con pipefail attivo
# quel fallimento "sporca" l'esito della pipeline anche se grep ha trovato il match cercato.
ENCODERS="$(ffmpeg -hide_banner -encoders 2>/dev/null)"
if grep -q h264_v4l2m2m <<< "$ENCODERS"; then
    ok "ffmpeg supporta l'encoder h264_v4l2m2m"
else
    bad "ffmpeg non ha il wrapper h264_v4l2m2m -- serve una build con supporto V4L2 M2M"
fi

ENC_DEV=""
if command -v v4l2-ctl >/dev/null 2>&1; then
    for d in /dev/video*; do
        [ -e "$d" ] || continue
        FMTS_OUT="$(v4l2-ctl -d "$d" --list-formats 2>/dev/null)"
        if grep -q "'H264'" <<< "$FMTS_OUT"; then
            ENC_DEV="$d"
            break
        fi
    done
fi
if [ -n "$ENC_DEV" ]; then
    ok "encoder H.264 hardware trovato su $ENC_DEV"
else
    warn "nessun device V4L2 espone un formato H264 in capture -- l'encoder hardware potrebbe non essere disponibile su questa scheda"
fi

section "Webcam"

if [ -z "$DEVICE" ]; then
    warn "nessun device indicato: uso \"$0 /dev/video0\" (o il tuo) per controlli più mirati"
else
    if [ -e "$DEVICE" ]; then
        ok "$DEVICE esiste"
        if FMTS=$(v4l2-ctl -d "$DEVICE" --list-formats-ext 2>&1); then
            if grep -q "'MJPG'" <<< "$FMTS"; then
                ok "$DEVICE supporta MJPEG (richiesto da capture_cam_hx.sh)"
                grep -A1 "'MJPG'" <<< "$FMTS" | grep "Size:" | sed 's/^/      /' | head -5 || true
            else
                warn "$DEVICE non sembra supportare MJPEG -- capture_cam_hx.sh non funzionerà così com'è"
            fi
        else
            bad "impossibile interrogare i formati di $DEVICE (device inesistente o driver non risponde)"
        fi
    else
        bad "$DEVICE non esiste"
    fi
fi

section "NDI Advanced SDK"

NDI_SDK="${NDI_SDK_DIR:-./ndi-adv-sdk}"
if [ -d "$NDI_SDK/include" ]; then
    ok "SDK trovato in $NDI_SDK"
    LIBDIRS=("$NDI_SDK"/lib/*/)
    if [ -d "${LIBDIRS[0]}" ]; then
        HOST_ARCH="$(uname -m)"
        MATCHED=0
        for d in "${LIBDIRS[@]}"; do
            d="${d%/}"
            ok "  libreria disponibile: $(basename "$d")"
            [[ "$(basename "$d")" == *"$HOST_ARCH"* ]] && MATCHED=1
        done
        if [ "$MATCHED" = 1 ]; then
            ok "una delle librerie corrisponde all'host ($HOST_ARCH)"
        else
            warn "nessuna libreria sembra corrispondere all'host ($HOST_ARCH) -- vedi NDI_SDK_LIBDIR nel README"
        fi
    else
        bad "$NDI_SDK/lib non contiene alcuna sottocartella per architettura"
    fi
else
    warn "SDK non trovato in $NDI_SDK -- imposta NDI_SDK_DIR se l'hai estratto altrove (vedi README)"
fi

section "Binari compilati"

for bin in ndi_hx_send ndi_hx_inspect; do
    if [ -x "./$bin" ]; then
        ok "$bin compilato"
        if command -v ldd >/dev/null 2>&1; then
            MISSING=$(ldd "./$bin" 2>&1 | grep "not found" || true)
            if [ -n "$MISSING" ]; then
                bad "$bin: librerie mancanti a runtime:"
                sed 's/^/      /' <<< "$MISSING"
            else
                ok "$bin: tutte le librerie si risolvono correttamente"
            fi
        fi
    else
        warn "$bin non compilato -- esegui ./build_hx.sh o ./build_inspect.sh"
    fi
done

section "Discovery NDI"

if [ -x "./ndi_hx_inspect" ]; then
    ok "cerco sorgenti NDI sulla rete per qualche secondo..."
    FOUND=$(timeout 4 ./ndi_hx_inspect "__doctor_nessuna_sorgente_dovrebbe_chiamarsi_cosi__" 2>&1 | grep -c "^  - " || true)
    if [ "${FOUND:-0}" -gt 0 ]; then
        ok "trovate $FOUND sorgenti NDI sulla rete (discovery funzionante)"
    else
        warn "nessuna sorgente NDI trovata -- normale se non ce n'è nessuna attiva sulla rete; se te ne aspetti almeno una controlla mDNS/firewall"
    fi
else
    warn "ndi_hx_inspect non compilato, salto il controllo di discovery"
fi

echo
echo "------------------------------------------------------------------------"
printf "Risultato: \033[32m%d ok\033[0m, \033[33m%d avvisi\033[0m, \033[31m%d problemi\033[0m\n" "$PASS" "$WARN" "$FAIL"
if [ "$FAIL" -gt 0 ]; then
    echo "Ci sono problemi da risolvere prima che il progetto funzioni correttamente."
fi
[ "$FAIL" -eq 0 ]
