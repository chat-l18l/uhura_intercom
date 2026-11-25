# Ontwerpdocument: Enterprise Low-Latency Intercom (`v1.0-draft`)

**Status:** Goedgekeurd (`GO` — Magic header `'UHUR'`)  
**Taal & Toolchain:** C23 (`-std=c23`), CMake + Ninja, Pixi (`linux-64` & `linux-aarch64`)  
**Doelplatformen:** Ubuntu 24.04+ op `x86_64` (Desktop/Server) en `aarch64` (Raspberry Pi 4/5, NVIDIA Jetson)  
**Standaard:** Conform [AGENTS.md](../AGENTS.md) (NASA Power of 10 + Zenoh-pico `_Owned` / `_Loaned`)

---

## 1. Functionele Samenvatting & Ontwerpdoelen

We bouwen een deterministisch, low-latency **Push-to-Talk (PTT) intercomsysteem** geïnspireerd op het wandpaneel van de *Star Trek Enterprise*:

1. **Multi-post adressering over UDP Unicast (LAN & WireGuard):**
   - Elke post heeft een vast `StationId` (`1..16`, bijv. *Brug*, *Machinekamer*, *Kantine*, *Ziekenboeg*) en een statische lijst van bekende peers (`IP:poort` op LAN of WireGuard `wg0`).
   - De spreker kiest **1, 2 of meer bestemmingen** (via een `TargetPeerMask` bitmask, inclusief *All-Call / Omroep*) en drukt op de PTT-knop.
   - Audio wordt **één keer** naar Opus geëncodeerd en als kleine UDP-unicast pakketten naar elke geselecteerde post verstuurd (geen multicast nodig, werkt 100% over WireGuard zonder MTU-fragmentatie).
2. **Vast Low-Latency Audioprofiel:**
   - **Sample rate:** `48.000 Hz`, **Mono** (`1` kanaal), `16-bit signed PCM` (`int16_t`).
   - **Framegrootte:** `10 ms` (`480` samples per pakket, `100` pakketten/s tijdens spreken).
   - **Codec:** `libopus` (`OPUS_APPLICATION_VOIP`, ~`32 kbps`, in-band FEC + Packet Loss Concealment).
   - **Eind-tot-eind latentiebudget (LAN / snelle WireGuard):** ~`25–45 ms` (10 ms capture + <1 ms Opus + netwerk + 10–20 ms jitterbuffer + 10 ms playout).
3. **Fase 1 = Half-Duplex PTT, Fase 2 = Full-Duplex + Echo Cancellation (AEC):**
   - In Fase 1 voorkomt een deterministische **PTT Duplex-Gate** akoestisch rondzingen op open speaker + microfoon zonder AEC.
   - De interne signaalpijplijn is al exact zó gesneden (synchrone `10 ms` speaker-referentie + microfoon-capture poort) dat we in Fase 2 een AEC-module kunnen inpluggen zonder de architectuur open te breken.

---

## 2. Systeemarchitectuur & Lagenscheiding (Regel 6: Strikte HAL)

Alle protocol-, codec-, mixer-, state-machine- en bufferlogica leeft in `src/core/` **zonder één enkele Linux-, socket- of ALSA-header**.

```text
┌──────────────────────────────────────────────────────────────────────────────┐
│                          platform/linux/ (Dunne OS-laag)                     │
│                                                                              │
│  ┌────────────────────┐    ┌─────────────────────┐    ┌───────────────────┐  │
│  │ audio_alsa.c       │    │ net_udp.c           │    │ control_input.c   │  │
│  │ (48kHz, 480 frames │    │ (Non-blocking UDP   │    │ (CLI/toets nu,    │  │
│  │  period, Pi/Jetson │    │  unicast IPv4/IPv6, │    │  GPIO knop op     │  │
│  │  & PipeWire-ALSA)  │    │  poll + timerfd)    │    │  Pi/Jetson straks)│  │
│  └────┬──────────▲────┘    └───────┬────────▲────┘    └─────────┬─────────┘  │
└───────┼──────────┼─────────────────┼────────┼───────────────────┼────────────┘
        │          │                 │        │                   │
   Capture     Playback           RX UDP    TX UDP          PttCommand +
   SPSC Ring   SPSC Ring         Datagram  Datagrams       TargetPeerMask
        │          │                 │        │                   │
┌───────▼──────────┴─────────────────▼────────┴───────────────────▼────────────┐
│                     src/core/ (100% Puur C23, 0 OS-headers)                  │
│                                                                              │
│  ┌────────────────────────────────────────────────────────────────────────┐  │
│  │ IntercomCore_Owned (Top-level expliciete context, ~295 KB statisch)    │  │
│  │                                                                        │  │
│  │  1. DuplexGate_Owned      (PTT mute/hangover nu -> AEC slot in Fase 2) │  │
│  │  2. OpusEncoder_Owned     (alignas statische buffer, opus_encoder_init)│  │
│  │  3. WireProto             (20B header serialize/parse + static_assert) │  │
│  │  4. RxStreamPool_Owned    (4x JitterBuffer_Owned + OpusDecoder_Owned)  │  │
│  │  5. PcmMixer              (Verzadigende int16 sommatie van sprekers)   │  │
│  │  6. PeerTable_Owned       (Max 16 posten, online-status & heartbeats)  │  │
│  └────────────────────────────────────────────────────────────────────────┘  │
└──────────────────────────────────────────────────────────────────────────────┘
```

### Threading & Real-Time Scheiding
Om te voorkomen dat een netwerk-hik of kernel-syscall ooit een kraakje in de audio veroorzaakt:
1. **Audio-thread (`audio_alsa.c`):** Doet uitsluitend blokje-voor-blokje (`480` frames = `10 ms`) overdracht tussen ALSA en twee lock-free Single-Producer Single-Consumer ringbuffers (`SpscPcmRing_Owned`). Geen mutexen, geen `malloc`, geen sockets.
2. **Core/Netwerk-loop (`main_linux.c`):** Wacht via `poll()` op de UDP-socket, een `10 ms` `timerfd` klok en het bedieningskanaal. Verwerkt inkomende UDP-pakketten direct in de juiste jitterbuffer en draait elke `10 ms` de deterministische `IntercomCore_tick_10ms()` stap.

---

## 3. Geheugenmodel & Nul-Allocatie Budget (Regel 1)

Zelfs `libopus` draait bij ons **zonder `malloc`**: in plaats van `opus_encoder_create()` gebruiken we `opus_encoder_get_size(1)` + `opus_encoder_init()` op een statisch uitgelijnd veld (`alignas(max_align_t) uint8_t storage[...]`), bewaakt met een runtime `assert()` bij opstarten.

| Onderdeel in `IntercomCore_Owned` | Aantal | Grootte per stuk | Totaal |
|---|---:|---:|---:|
| `OpusEncoder_Owned` (mono, statische state) | 1 | 64 KB | 64 KB |
| `OpusDecoder_Owned` (mono, statische state) | 4 | 40 KB | 160 KB |
| `JitterBuffer_Owned` (32 slots × 268 B) | 4 | ~8,6 KB | ~34,4 KB |
| `SpscPcmRing_Owned` (Capture + Playback, 16 frames × 960 B) | 2 | ~15,4 KB | ~30,8 KB |
| `PeerTable_Owned` (16 peers) + PTT/Session state | 1 | ~2,0 KB | ~2,0 KB |
| **Totaal statisch geheugen (`IntercomCore_Owned`)** | | | **~291 KB** |

> **Waarom 4 `RxStreamSlot_Owned` decoders op 16 mogelijke posten?**  
> Als post *Brug* en post *Ziekenboeg* toevallig tegelijkertijd naar *Machinekamer* roepen, hebben ze elk een eigen `SeqNum`-reeks en eigen Opus-decoderstate nodig. Met 4 vaste ontvangst-slots (dynamisch gekoppeld aan actieve sprekers op basis van `StationId` en vrijgegeven na `TALK_END`/timeout) kunnen maximaal 4 posten **tegelijk door elkaar** praten en netjes gemixt worden, binnen een vast geheugenbudget van <300 KB.

---

## 4. Netwerk Wire-Protocol (`core/wire_proto.h`)

Elk UDP-pakket begint met een vaste **20-byte header** in expliciete big-endian (netwerk) byte-volgorde (geen `__attribute__((packed))` pointer-casts, dus 100% veilig en identiek op `x86_64` en `aarch64`).

```text
 0                   1                   2                   3
 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|      'U'      |      'H'      |      'U'      |      'R'      |  Bytes 0..3: Magic ("UHUR")
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|  Version (1)  |  PacketKind   | SenderStation |     Flags     |  Bytes 4..7
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                         TalkSessionId                         |  Bytes 8..11 (uint32)
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                            SeqNum                             |  Bytes 12..15 (uint32)
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|        TargetPeerMask         |          PayloadLen           |  Bytes 16..19 (2x uint16)
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|               Opus Payload (0 .. 256 bytes) ...               |  Bytes 20..N
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
```

- **`PacketKind` (`enum PacketKind : uint8_t`):**
  - `PACKET_AUDIO (1)`: Bevat 10 ms Opus-spraak (`PayloadLen` typisch `35..60` bytes -> totaal UDP-pakket ~`60..80` bytes, ruim onder WireGuard's `1420` B MTU; bandbreedte ~`90 kbps` per bestemming).
  - `PACKET_TALK_START (2)`: Wordt direct verstuurd bij indrukken van de grote PTT-knop. Kan op ontvangende posten een optionele *Enterprise intercom-chime* starten en bereidt de jitterbuffer voor. *(Gaat dit UDP-pakket verloren, dan opent het eerste `PACKET_AUDIO` pakket alsnog direct de stream!)*
  - `PACKET_TALK_END (3)`: Wordt bij loslaten van de PTT-knop (2× kort achter elkaar tegen verlies) verstuurd om de spreekbeurt aan de overkant strak te beëindigen zonder op een buffer-timeout te wachten.
  - `PACKET_HEARTBEAT (4)`: Elke `5 seconden` (`PayloadLen = 0`). Houdt de WireGuard-tunnel en NAT-tabellen open en toont per post of deze online is.
- **`TalkSessionId` (`uint32_t`):** Hoogt met `1` op bij elke nieuwe druk op de PTT-knop. Hiermee ziet de jitterbuffer direct het verschil tussen een verlaat pakket uit een vorige zin en pakket `0` van een nieuwe oproep.
- **`TargetPeerMask` (`uint16_t`):** Bit `0..15` corresponderen met `StationId 1..16`. Bitmask `0xFFFF` of meervoudige bits geeft een groepsomroep aan (bijv. *Machinekamer + Kantine*).

---

## 5. C23 Types, Ownership & Functie-Signaturen (Regels 2, 3, 5 & 7)

### 5.1 Strong Types & Slices (`include/core/types.h`)

```c
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <assert.h>

/* C23 constanten met compile-time bewijzen */
constexpr uint32_t INTERCOM_SAMPLE_RATE_HZ     = 48000U;
constexpr uint16_t INTERCOM_SAMPLES_PER_FRAME  = 480U;   /* 10 ms @ 48 kHz */
constexpr size_t   INTERCOM_MAX_OPUS_BYTES     = 256U;
constexpr size_t   INTERCOM_WIRE_HEADER_BYTES  = 20U;
constexpr size_t   INTERCOM_MAX_WIRE_BYTES     = INTERCOM_WIRE_HEADER_BYTES + INTERCOM_MAX_OPUS_BYTES;
constexpr uint8_t  INTERCOM_MAX_PEERS          = 16U;
constexpr uint8_t  INTERCOM_MAX_RX_STREAMS     = 4U;

static_assert(INTERCOM_SAMPLES_PER_FRAME * 100U == INTERCOM_SAMPLE_RATE_HZ,
              "Frame size must equal exactly 10 ms at 48 kHz");
static_assert(INTERCOM_MAX_WIRE_BYTES <= 512U,
              "Wire packet must stay well below WireGuard MTU (1280/1420 bytes)");

/* Domein Strong Types (Regel 3: geen naked primitives) */
typedef struct { uint8_t  id;   } StationId;       /* Geldig: 1 .. 16 */
typedef struct { uint16_t mask; } TargetPeerMask;  /* Bit (id - 1) per station */
typedef struct { uint32_t id;   } TalkSessionId;
typedef struct { uint32_t num;  } SeqNum;
typedef struct { uint32_t bps;  } BitrateBps;
typedef struct { uint64_t us;   } MonotonicTimeUs;
typedef struct { uint16_t port; } UdpPort;
typedef struct { uint16_t frames; } FrameCount;

/* Geleende Slices (Regel 2 & 3: mogen NOOIT in een *_Owned struct worden bewaard!) */
typedef struct {
    const uint8_t *data;
    size_t len;
} ByteSlice_Loaned;

typedef struct {
    uint8_t *data;
    size_t len;
    size_t capacity;
} ByteSliceMut_Loaned;

typedef struct {
    const int16_t *samples;
    FrameCount frame_count;
} PcmMonoSlice_Loaned;

typedef struct {
    int16_t *samples;
    FrameCount frame_count;
} PcmMonoSliceMut_Loaned;
```

### 5.2 Eigenaarschap & Move-Semantiek (`EncodedFrame_Owned`)

```c
typedef struct {
    uint8_t bytes[INTERCOM_MAX_OPUS_BYTES];
    uint16_t len;
    SeqNum seq;
    TalkSessionId session;
    bool is_valid;
} EncodedFrame_Owned;

/* Leen read-only byte-view van een geldig frame */
[[nodiscard]] ByteSlice_Loaned EncodedFrame_loan(const EncodedFrame_Owned *self);

/* Verplaats eigenaarschap naar dst en maak src direct ongeldig (*src = {0}) */
void EncodedFrame_move(EncodedFrame_Owned * restrict dst, EncodedFrame_Owned * restrict src);
```

### 5.3 Parameter Object Pattern op Modulegrenzen (Regel 7)

Geen enkele functie heeft twee opeenvolgende parameters van hetzelfde type:

```c
typedef enum : uint8_t {
    FEC_DISABLED = 0,
    FEC_ENABLED  = 1,
} InbandFecMode;

typedef struct {
    BitrateBps    bitrate;
    InbandFecMode fec_mode;
} OpusEncoderConfigParams;

[[nodiscard]] CodecResult OpusEncoder_init(
    OpusEncoder_Owned *ctx,
    OpusEncoderConfigParams params
);

typedef struct {
    StationId      sender;
    TargetPeerMask targets;
    TalkSessionId  session;
    SeqNum         seq;
    ByteSlice_Loaned opus_payload;
} WireAudioSerializeParams;

[[nodiscard]] WireResult WireProto_serialize_audio(
    ByteSliceMut_Loaned out_datagram,
    WireAudioSerializeParams params,
    size_t *out_written_len
);
```

---

## 6. PTT State Machine & Voorbereiding op Fase 2 (Echo Cancellation)

### 6.1 Fase 1: PTT State Machine (`DuplexGate_Owned`)
Op een luidspreker-intercom zonder AEC veroorzaakt gelijktijdig afspelen en opnemen rondzingen. Daarom kent `DuplexGate_Owned` drie toestanden:

1. **`GATE_LISTENING` (Standaard / Rust):**
   - Luidspreker speelt gemixte inkomende oproepen af.
   - Microfoon-input wordt gemute; er gaan geen audiopakketten het netwerk op.
2. **`GATE_TRANSMITTING` (Grote PTT-knop ingedrukt):**
   - Bij overgang `LISTENING -> TRANSMITTING`: verhoog `TalkSessionId`, reset `SeqNum = 0`, verstuur `PACKET_TALK_START` naar `TargetPeerMask`.
   - Luidspreker wordt lokaal gedempt (zodat inkomend geluid niet in de open microfoon lekt).
   - Microfoon-frames (`480` samples) gaan elke `10 ms` naar `OpusEncoder_Owned` -> UDP unicast fan-out.
3. **`GATE_HANGOVER` (PTT net losgelaten, duurt `30 ms` = 3 frames):**
   - Verstuur `PACKET_TALK_END` naar `TargetPeerMask`.
   - Houdt speaker nog `30 ms` gedempt terwijl kamer-nagalm uitsterft, en keert daarna terug naar `GATE_LISTENING`.

### 6.2 Hoe Fase 2 (Echo Cancellation) straks naadloos inplugt
Voor Acoustic Echo Cancellation (bijv. via `speexdsp` of WebRTC AEC) moet het algoritme exact weten welk signaal net naar de speaker is gestuurd (`far_end_ref`) om die echo uit het microfoonsignaal (`near_end_mic`) te filteren.

Daarom krijgt onze front-end module nu al deze exacte signatuur:

```c
typedef struct {
    PcmMonoSlice_Loaned    speaker_reference; /* Wat naar de speaker gaat (480 samples) */
    PcmMonoSliceMut_Loaned mic_capture_inout; /* Wat van de mic komt -> gefilterd uit */
    PcmMonoSliceMut_Loaned speaker_out;       /* Wat daadwerkelijk naar ALSA playback gaat */
} AudioFrontEndFrameParams;

void DuplexGate_process_10ms(DuplexGate_Owned *ctx, AudioFrontEndFrameParams params);
```
In Fase 1 dempt deze functie simpelweg `speaker_out` of `mic_capture_inout` op basis van de PTT-toestand. In Fase 2 plaatsen we achter exact dezelfde poort de AEC-filterstap zonder `IntercomCore`, de netwerkcode of de ALSA-adapter te hoeven verbouwen.

---

## 7. Build-, Dependency- & Teststrategie (Pixi + CMake)

### 7.1 `pixi.toml` opzet (`linux-64` & `linux-aarch64`)
- **Dependencies uit `conda-forge`:** `cmake`, `ninja`, `clang` (of `gcc`), `pkg-config`, `libopus`, `alsa-lib`.
- **Audio-compatibiliteit voor zowel Pi/Jetson als Ubuntu Desktop:**
  - Op een **headless Raspberry Pi of Jetson** praat `audio_alsa.c` rechtstreeks met het ALSA-device (bijv. `plughw:0,0` of configureerbare `--capture-dev` / `--playback-dev` naam).
  - Op een **Ubuntu 24.04 Desktop met PipeWire** gebruikt ALSA het `"default"` device via PipeWire's ALSA-plugin. We zorgen dat het build/run-script zo nodig het systeem-pluginpad (`/usr/lib/{x86_64,aarch64}-linux-gnu/alsa-lib`) meeneemt zodat Pixi's `libasound` naadloos met Ubuntu's PipeWire samenwerkt.

### 7.2 Beoogde Mapstructuur & Bouwfasen

```text
.
├── AGENTS.md
├── README.md
├── docs/
│   └── DESIGN.md                  # Dit ontwerpdocument
├── pixi.toml                      # Multi-arch dependencies & build/test tasks
├── CMakeLists.txt                 # Strikte C23 flags (-Wall -Wextra -Werror -Wconversion -Wshadow)
├── include/
│   ├── core/
│   │   ├── types.h                # Strong types, _Loaned slices, static_asserts
│   │   ├── wire_proto.h           # 20-byte UHUR protocol serialize/parse
│   │   ├── opus_codec.h           # Zero-malloc Opus encoder/decoder wrappers (_Owned)
│   │   ├── jitter_buffer.h        # Vaste-capaciteit jitterbuffer (_Owned)
│   │   ├── spsc_pcm_ring.h        # Lock-free PCM ringbuffer (_Owned)
│   │   ├── duplex_gate.h          # PTT state machine & AEC-ready front-end (_Owned)
│   │   └── intercom_core.h        # Top-level orchestrator + peer fan-out (_Owned)
│   └── platform/
│       └── linux_hal.h            # Smalle interface naar UDP, ALSA en PTT-bediening
├── src/
│   ├── core/                      # 100% OS-vrije C23 implementaties
│   └── platform/linux/
│       ├── audio_alsa.c           # ALSA 48kHz 10ms capture & playback
│       ├── net_udp.c              # Non-blocking UDP unicast over LAN / WireGuard
│       ├── control_cli.c          # Interactieve PTT & post-selectie in terminal
│       └── main_linux.c           # Config-parsing, statische context & poll-loop
└── tests/
    ├── test_wire_proto.c          # Roundtrip, corruptie & grensgevallen
    ├── test_jitter_buffer.c       # Reordering, packet loss (PLC), nieuwe TalkSessionId
    ├── test_opus_codec.c          # 480-sample encode/decode + PLC zonder malloc
    └── test_intercom_loopback.c   # Volledige virtuele 3-posten test (Brug -> Machinekamer + Kantine)
```

### 7.3 Gefaseerd Implementatieplan (na goedkeuring)
1. **Stap 1 — Build-skelet & Pure Basis (`pixi.toml`, `CMakeLists.txt`, `types.h`, `wire_proto`, `spsc_pcm_ring`):** Inclusief de eerste unit-tests onder strikte `-Werror` flags.
2. **Stap 2 — Audio-kern (`opus_codec`, `jitter_buffer`, `duplex_gate`, `intercom_core`):** Inclusief een gesimuleerde 3-posten unit-test (*Brug* roept *Machinekamer + Kantine* aan met gesimuleerd pakketverlies en jitter) — nog volledig zonder geluidskaart of root/netwerk.
3. **Stap 3 — Linux HAL & CLI-app (`net_udp.c`, `audio_alsa.c`, `control_cli.c`, `main_linux.c`):** Werkende intercom-binary waarmee je tussen twee terminals/machines/WireGuard-nodes op spatie/toets kunt drukken om naar 1 of meerdere posten te spreken.
