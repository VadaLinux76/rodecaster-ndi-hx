#!/bin/bash
# Compila ndi_hx_send collegandolo all'NDI Advanced SDK.
#
# L'SDK NON e' incluso in questo repo (e' proprietario di Vizrt/NDI, licenza gratuita ma non
# ridistribuibile) - vedi il README per come scaricarlo. Estrallo in una cartella e passane il
# path in NDI_SDK_DIR se non e' ./ndi-adv-sdk accanto a questo script, es.:
#   NDI_SDK_DIR=/opt/ndi-advanced-sdk ./build_hx.sh

set -euo pipefail
cd "$(dirname "$0")"

NDI_SDK="${NDI_SDK_DIR:-./ndi-adv-sdk}"

if [ ! -d "$NDI_SDK/include" ]; then
    echo "SDK NDI Advanced non trovato in $NDI_SDK (vedi README per scaricarlo)." >&2
    echo "Imposta NDI_SDK_DIR se l'hai messo altrove." >&2
    exit 1
fi

# La cartella lib contiene una sottocartella per architettura (es. x86_64-linux-gnu,
# aarch64-rpi4-linux-gnueabi su Raspberry Pi OS/Ubuntu). La prendiamo automaticamente.
LIBDIR=$(find "$NDI_SDK/lib" -mindepth 1 -maxdepth 1 -type d | head -1)
if [ -z "$LIBDIR" ]; then
    echo "Nessuna sottocartella lib/<arch> trovata sotto $NDI_SDK/lib" >&2
    exit 1
fi

g++ -std=c++17 -O2 -Wall \
    -I"$NDI_SDK/include" \
    -L"$LIBDIR" \
    -Wl,-rpath,"$(realpath "$LIBDIR")" \
    -o ndi_hx_send ndi_hx_send.cpp \
    -lndi_advanced -ldl -lpthread

echo "Build OK -> ./ndi_hx_send (linkato contro $LIBDIR)"
