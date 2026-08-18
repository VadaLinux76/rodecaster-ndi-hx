#!/bin/bash
# Compila ndi_hx_inspect, il tool diagnostico che si collega a una sorgente NDI in modalita'
# COMPRESSED e stampa la struttura dei pacchetti ricevuti (utile per confrontare il proprio
# sender con una sorgente HX reale, es. RODE Capture su smartphone). Stesso SDK di build_hx.sh.

set -euo pipefail
cd "$(dirname "$0")"

NDI_SDK="${NDI_SDK_DIR:-./ndi-adv-sdk}"

if [ ! -d "$NDI_SDK/include" ]; then
    echo "SDK NDI Advanced non trovato in $NDI_SDK (vedi README per scaricarlo)." >&2
    exit 1
fi

LIBDIR=$(find "$NDI_SDK/lib" -mindepth 1 -maxdepth 1 -type d | head -1)
if [ -z "$LIBDIR" ]; then
    echo "Nessuna sottocartella lib/<arch> trovata sotto $NDI_SDK/lib" >&2
    exit 1
fi

g++ -std=c++17 -O2 -Wall \
    -I"$NDI_SDK/include" \
    -L"$LIBDIR" \
    -Wl,-rpath,"$(realpath "$LIBDIR")" \
    -o ndi_hx_inspect ndi_hx_inspect.cpp \
    -lndi_advanced -ldl -lpthread

echo "Build OK -> ./ndi_hx_inspect (linkato contro $LIBDIR)"
