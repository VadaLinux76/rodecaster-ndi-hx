# shellcheck shell=bash
# Da "sourceare" (non eseguire) dagli script di build dopo aver impostato NDI_SDK. Popola la
# variabile LIBDIR con la sottocartella lib/<arch> giusta per l'host corrente, invece di
# prendere semplicemente "la prima trovata" (code review: Codex, fase 3 - "rilevare
# architettura e ABI del compilatore, selezionare esplicitamente la libreria corretta": con un
# SDK multi-architettura estratto in un'unica cartella, la prima trovata potrebbe non
# corrispondere all'host e produrre un binario che non parte, o non parte affatto).
#
# Override manuale: imposta NDI_SDK_LIBDIR per saltare del tutto il rilevamento automatico.

if [ -n "${NDI_SDK_LIBDIR:-}" ]; then
    LIBDIR="$NDI_SDK_LIBDIR"
    if [ ! -d "$LIBDIR" ]; then
        echo "NDI_SDK_LIBDIR punta a una cartella inesistente: $LIBDIR" >&2
        exit 1
    fi
else
    CANDIDATES=("$NDI_SDK"/lib/*/)
    if [ ! -d "${CANDIDATES[0]}" ]; then
        echo "Nessuna sottocartella lib/<arch> trovata sotto $NDI_SDK/lib" >&2
        exit 1
    fi

    if [ ${#CANDIDATES[@]} -eq 1 ]; then
        LIBDIR="${CANDIDATES[0]%/}"
    else
        # Piu' di una sottocartella (SDK multi-architettura): cerca quella il cui nome
        # contiene l'architettura host rilevata (es. "aarch64" o "x86_64").
        HOST_ARCH="$(uname -m)"
        LIBDIR=""
        for d in "${CANDIDATES[@]}"; do
            d="${d%/}"
            if [[ "$(basename "$d")" == *"$HOST_ARCH"* ]]; then
                LIBDIR="$d"
                break
            fi
        done
        if [ -z "$LIBDIR" ]; then
            echo "SDK multi-architettura: nessuna sottocartella lib/ corrisponde all'host ($HOST_ARCH)." >&2
            echo "Trovate: ${CANDIDATES[*]}" >&2
            echo "Imposta NDI_SDK_LIBDIR per sceglierne una esplicitamente." >&2
            exit 1
        fi
    fi
fi
