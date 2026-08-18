# rodecaster-ndi-hx

[![License: MIT](https://img.shields.io/github/license/VadaLinux76/rodecaster-ndi-hx)](LICENSE)
[![CI](https://github.com/VadaLinux76/rodecaster-ndi-hx/actions/workflows/ci.yml/badge.svg)](https://github.com/VadaLinux76/rodecaster-ndi-hx/actions/workflows/ci.yml)

🇮🇹 Idea e progetto italiani, nati dal debugging di una serata su un RØDECaster Video vero.
Leggi questo README anche in [🇬🇧 English](README.en.md).

Trasforma una webcam USB collegata a un Raspberry Pi 4 in una sorgente **NDI|HX** vera (non
NDI standard/SpeedHQ), usando l'encoder H.264 hardware del Pi4. Nato per alimentare un
[RØDECaster Video](https://rode.com/en-us/rodecaster/rodecaster-video), che accetta solo
NDI|HX2+ certificato — ma qualunque receiver NDI|HX dovrebbe andare bene.

```
webcam USB (MJPEG/YUYV) --ffmpeg--> H.264 (encoder hardware bcm2835-codec) --ndi_hx_send--> NDI|HX
```

## Perché non uno dei tanti "webcam to NDI" già in giro

I tool "NDI to webcam" comuni mandano NDI *standard* (compresso internamente in SpeedHQ dalla
libreria), che è molto più pesante in banda e **non è quello che un device NDI|HX-only si
aspetta**. Per fare vero NDI|HX serve mandare pacchetti H.264/HEVC già compressi usando le API
di basso livello dell'**NDI Advanced SDK** — capacità non documentata benissimo, e con un paio
di trabocchetti non ovvi (vedi sotto). Questo repo è il risultato di quella sessione di
debugging, in modo che chi ci si scontra dopo non debba rifarla da zero.

## Requisiti

- Raspberry Pi 4 (serve l'encoder H.264 hardware `bcm2835-codec`, esposto da ffmpeg come
  `h264_v4l2m2m`) — su altro hardware serve adattare `capture_cam_hx.sh`
- Una webcam USB che parli MJPEG o YUYV via V4L2 (`v4l2-ctl --list-formats-ext`)
- `ffmpeg` con supporto `h264_v4l2m2m` (di serie su Raspberry Pi OS / Ubuntu per Pi recenti)
- **NDI Advanced SDK** — *non incluso in questo repo* (proprietario Vizrt/NDI, licenza
  d'uso gratuita ma non ridistribuibile). Scaricalo da
  [ndi.video](https://ndi.video/for-developers/ndi-advanced/) (richiede una registrazione
  gratuita), estrallo, e passa il path a `build_hx.sh`/`build_inspect.sh` via `NDI_SDK_DIR` se
  non lo metti in `./ndi-adv-sdk` accanto agli script.

  Nota: la versione liberamente scaricabile dell'SDK è marcata "development use" e si
  autolimita a stream di 30 minuti — per uso continuativo/commerciale serve contattare
  `licensing@ndi.video`. Nei test descritti sotto non abbiamo mai raggiunto quel limite.

## Build

```bash
./build_hx.sh        # -> ndi_hx_send (il sender)
./build_inspect.sh    # -> ndi_hx_inspect (tool diagnostico, opzionale)
```

Se l'SDK ha più sottocartelle `lib/<arch>` (un pacchetto multi-architettura), viene scelta
automaticamente quella che corrisponde all'host (`uname -m`); per bypassare il rilevamento e
sceglierne una esplicitamente, imposta `NDI_SDK_LIBDIR=/path/esatto`.

In alternativa, [CMake](#build-con-cmake) offre gli stessi target più `ctest`/`install`.

## Architettura del codice

Il parsing dello stream Annex-B e la ricostruzione delle access unit vivono in
`annexb.h`/`annexb.cpp`, un modulo **senza alcuna dipendenza dall'SDK NDI** — `ndi_hx_send.cpp`
resta un wrapper sottile che lo usa e traduce ogni frame assemblato in un pacchetto
`NDIlib_compressed_packet_t`. La separazione esiste apposta per rendere testabile in CI la
parte più delicata (parsing/assemblaggio) senza aver bisogno dell'SDK proprietario, che qui non
è scaricabile automaticamente.

```bash
# Test unitari del modulo annexb (nessuna dipendenza, gira ovunque con solo g++/clang++)
g++ -std=c++17 -Wall -Wextra -Wpedantic -o test_annexb tests/test_annexb.cpp annexb.cpp
./test_annexb

# stessa cosa con AddressSanitizer + UndefinedBehaviorSanitizer (quello che gira in CI)
g++ -std=c++17 -O1 -g -fsanitize=address,undefined -o test_annexb_san tests/test_annexb.cpp annexb.cpp
./test_annexb_san
```

### Build con CMake

Alternativa agli script bash sopra, con target `build`/`test`/`install` standard. Gli script
restano comunque il modo più semplice per chi non vuole installare CMake.

```bash
# solo il modulo annexb + i test (nessun SDK richiesto)
cmake -B build && cmake --build build && ctest --test-dir build --output-on-failure

# anche ndi_hx_send/ndi_hx_inspect, collegati contro l'SDK indicato
cmake -B build -DNDI_SDK_DIR=/path/to/ndi-adv-sdk && cmake --build build
```

## Uso

```bash
./capture_cam_hx.sh <device> <larghezza> <altezza> <fps> <nome_ndi> [bitrate] [gop_frame] [profilo] [livello]

# esempio:
./capture_cam_hx.sh /dev/video0 1920 1080 30 "Pi4 Cam HX" 8M 2

# <fps> accetta anche una frazione N/D (es. 30000/1001, il classico "29.97fps" NTSC) — solo se
# la webcam supporta davvero quel preciso frame interval, verificalo con
# `v4l2-ctl --list-formats-ext`
./capture_cam_hx.sh /dev/video0 1920 1080 30000/1001 "Pi4 Cam HX" 8M 2
```

Per farlo partire al boot, copia `ndi-webcam-send.service` in `/etc/systemd/system/`
(sostituendo i placeholder `<user>` e il path della tua webcam), poi:

```bash
sudo systemctl daemon-reload
sudo systemctl enable --now ndi-webcam-send.service
```

### Riavvio periodico (limite di 30 minuti dell'SDK "development")

Se usi la versione liberamente scaricabile dell'NDI Advanced SDK (vedi Requisiti sopra), lo
stream si autolimita a 30 minuti. `ndi-webcam-restart.service`/`.timer` sono un timer systemd
di esempio che riavvia `ndi-webcam-send.service` ogni 20 minuti (comoda distanza di sicurezza
sotto i 30), a costo di qualche secondo di interruzione visibile sul receiver ad ogni giro.
Con una licenza commerciale (che rimuove il limite) non serve.

```bash
sudo cp ndi-webcam-restart.service ndi-webcam-restart.timer /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now ndi-webcam-restart.timer
systemctl list-timers ndi-webcam-restart.timer  # controlla quando scatterà il prossimo giro
```

## `ndi_hx_inspect`: verificare cosa arriva davvero

```bash
./ndi_hx_inspect "<nome sorgente NDI>"
```

Si collega come receiver NDI in modalità `COMPRESSED` e stampa la struttura reale dei pacchetti
ricevuti (FourCC, keyframe, dimensioni, extra_data). Utile sia per debuggare il proprio sender
sia per ispezionare come si comporta una sorgente HX certificata reale (es. l'app RODE Capture
su smartphone) e confrontarla.

## Autodiagnosi

```bash
./doctor.sh /dev/video0
```

Controlla tutti i prerequisiti (ffmpeg, encoder hardware, formati della webcam, SDK NDI,
binari compilati, discovery di rete) e segnala cosa manca. Solo controlli in lettura: non
tocca mai la webcam in streaming, quindi è sicuro da lanciare anche a servizio già attivo.

## I 6 bug non ovvi (la parte utile per chi arriva da una ricerca disperata)

Il sintomo di partenza, su un receiver certificato reale (RØDECaster Video): connessione NDI
stabilita normalmente, tally funzionante, ma video rifiutato con
`unsupported, resolution 0x0, frame rate 23, format N/A`. Un tool di verifica scritto in casa
(che si limita a leggere i byte senza decodificarli davvero) può sembrare che tutto funzioni
anche quando il contenuto è semanticamente sbagliato — motivo per cui questi bug sono
sopravvissuti a lungo ai test locali.

1. **FourCC `*_lowest_bandwidth` non è "lo stesso video a bitrate più basso"**: per specifica
   NDI è riservato a un secondo stream di *anteprima a risoluzione fissa 640px*. Per il flusso
   principale a piena risoluzione va sempre usato `*_highest_bandwidth`, qualunque sia la
   risoluzione reale che stai inviando. (Inizialmente sembrava un problema di licenza dell'SDK,
   dato che i frame taggati `lowest_bandwidth` non arrivavano mai a nessun receiver — la causa
   vera è proprio che l'SDK li scarta se non rispettano il vincolo dei 640px.)

2. **La causa reale del "resolution 0x0 / format N/A"**: i dati video e l'extra_data (SPS/PPS)
   vanno in formato **Annex-B** (start code `00 00 00 01`), *non* AVCC/length-prefixed (lo
   stile MP4/avcC). La [documentazione ufficiale](https://docs.ndi.video/all/developing-with-ndi/advanced-sdk/using-h.264-h.265-and-aac-codecs/sending-video-frames)
   lo dice esplicitamente, ma è facile non trovarla e assumere AVCC per abitudine.

3. `NDIlib_video_frame_v2_t.picture_aspect_ratio` **non può essere 0** per stream compressi —
   va messo il rapporto reale (es. `(float)width / height`). Con 0 un receiver strict può
   leggere dimensioni non valide.

4. `frame.timecode` deve essere il **PTS reale** del pacchetto, non il sentinel
   `NDIlib_send_timecode_synthesize`.

5. L'encoder hardware del Pi4 usa di default **H.264 High Profile**. Impostarlo su un profilo
   più compatibile (es. Main) con `v4l2-ctl --set-ctrl` *prima* di avviare ffmpeg **non ha
   alcun effetto**: i device V4L2 mem2mem sono stateless, ogni `open()` (compreso quello di
   ffmpeg) crea un'istanza indipendente con i default hardware. Il profilo va passato a ffmpeg
   stesso, come **valore numerico** (`-profile:v 77` per Main, `100` per High — i nomi stringa
   tipo `"main"` non sono accettati da `h264_v4l2m2m`).

6. `h264_v4l2m2m` ignora `-g` da solo: servono keyframe forzati esplicitamente con
   `-force_key_frames "expr:eq(mod(n,GOP),0)"`. Nella config qui il GOP è tenuto molto corto
   (un keyframe ogni 2 frame, ~66ms) perché l'SDK NDI segnala a runtime che per essere
   "NDI|HX compliant" un I-frame deve arrivare entro 100ms da quando un receiver si connette.

## Limitazioni

- **Il Pi4 viaggia stabilmente intorno al 95% di CPU** (misurato a 1920x1080@30) mentre lo
  stream è attivo. Il colpevole non è l'encoder H.264 (quello è hardware): è il decode
  software del MJPEG in ingresso dalla webcam più la conversione di colorspace fatte da
  ffmpeg su CPU, che da sole occupano stabilmente 2+ core su 4. Su un Pi4 dedicato solo a
  questo va bene, ma non aspettarti margine per fare altro sulla stessa macchina.
- Il Pi4 non ha encoder HEVC hardware (solo decode) — quindi qui si fa NDI|HX2 (H.264), non
  HX3/HEVC. HX3 via software encoding sarebbe probabilmente troppo pesante per il Pi4 in
  tempo reale (si aggiungerebbe all'uso di CPU già alto del punto sopra).
- Testato con una singola webcam USB MJPEG 1920x1080@30 e un RØDECaster Video. Altre
  combinazioni webcam/receiver potrebbero avere ulteriori sorprese — issue e PR benvenute.
- La CI (badge sopra) valida gli script bash (`shellcheck`) e il modulo `annexb.h`/`.cpp`
  (build + test su GCC e Clang, con `-Wall -Wextra -Wpedantic -Wconversion -Werror` e sotto
  AddressSanitizer/UndefinedBehaviorSanitizer). **Non** compila invece `ndi_hx_send.cpp` /
  `ndi_hx_inspect.cpp` per intero: l'SDK NDI Advanced non è scaricabile automaticamente in CI
  (richiede registrazione manuale su ndi.video), e sono proprio quei due file gli unici a
  dipenderne — è anche il motivo per cui la logica delicata è stata estratta in `annexb`.
- **Access unit multi-slice**: `ndi_hx_send` le ricompone correttamente (via Access Unit
  Delimiter quando presente, o leggendo `first_mb_in_slice` dallo slice header quando assente —
  vedi `annexb.h`). Questo introduce una latenza di un access unit: un frame viene inviato solo
  quando arriva la prima slice di quello successivo, o a fine stream. Con l'encoder hardware del
  Pi4 (`h264_v4l2m2m`, che non fa slicing) la latenza aggiuntiva è quindi trascurabile.
- La CI (badge sopra) verifica anche la build via CMake, ma solo la parte che non dipende
  dall'SDK NDI (modulo `annexb` + test): `ndi_hx_send`/`ndi_hx_inspect` via CMake richiedono
  `-DNDI_SDK_DIR=...` e non vengono quindi compilati automaticamente in CI, per lo stesso
  motivo del punto sopra.

## Debug

`ndi_hx_send` può scrivere in `/tmp/hx_debug_*` i byte grezzi del primo keyframe (SPS/PPS,
extra_data, pacchetto NDI completo) per ispezionarli con `ffprobe`/`hexdump` fuori banda —
utile se qualcosa non torna con un nuovo encoder/webcam. Disattivato di default: attivalo con
la variabile d'ambiente `NDI_HX_DEBUG` (qualunque valore, basta che sia definita):

```bash
NDI_HX_DEBUG=1 ./capture_cam_hx.sh /dev/video0 1920 1080 30 "Pi4 Cam HX"
```

## Licenza

Questo codice è rilasciato sotto licenza MIT (vedi `LICENSE`). L'NDI Advanced SDK necessario
per compilarlo **non è incluso** ed è soggetto alla licenza propria di Vizrt/NDI
(vedi [ndi.video](https://ndi.video/for-developers/ndi-advanced/)).

## Per assistenti/agenti AI

Due file pensati per essere letti da un LLM, secondo la convenzione [llms.txt](https://llmstxt.org/):

- [`llms.txt`](llms.txt) — versione leggera: solo indice e link ai file rilevanti, con una
  riga di descrizione ciascuno
- [`llms-full.txt`](llms-full.txt) — versione completa: contenuto integrale di README,
  sorgenti, script e config consolidato in un unico file, senza bisogno di seguire link o
  clonare la repo
