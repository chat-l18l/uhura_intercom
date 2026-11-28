# Functie-Ontwerp & Systeemimpact: "Roep & Luister" (Welzijnsoproep met 30s Antwoordvenster)

**Status:** Geïmplementeerd & Getest op Linux én Windows 11 (`tests/test_care_listen.c`)  
**Aanleiding:** Veiligheid thuis bij valgevaar (wanneer telefoon in een andere kamer ligt en smartwatch/alarmketting op de lader ligt of in de badkamer niet wordt gedragen).

---

## 1. Het Scenario & Waarom Dit Anders Werkt dan een Telefoon of Noodknop

Wanneer iemand in huis valt (bijvoorbeeld in de gang, slaapkamer of badkamer), faalt bestaande techniek vaak precies op dat moment: een smartphone ligt op tafel in de woonkamer, een smartwatch ligt aan de oplader, en zelfs een halszender wordt bij douchen of slapen vaak afgedaan.

De **Welzijnsoproep ("Roep & Luister")** draait de rollen om:
1. Een vertrouwd familielid (*Edwin* of *Zusje*) merkt dat het stil is of wil even checken, en start een **Welzijnsoproep** naar het huis van *Moeder* (één centraal kastje of meerdere gekoppelde kastjes in bijv. woonkamer én overloop/badkamer).
2. In het hele huis van Moeder schakelt het speaker-volume via een **Nood-Volume Override** tijdelijk naar 100% verstaanbaar noodvolume (ook als het kastje voor de avond zachter was gezet). Er klinkt eerst een extra duidelijke aankondigingstoon en vervolgens luid en helder de stem van Edwin: *"Ma, gaat alles goed daar? Roep eens wat of klop even op de deur!"*
3. Zodra Edwin zijn spreekknop loslaat, klinkt bij Moeder een herkenbare *"Ik luister nu"*-toon, gaat het rode **MIC AAN**-lampje branden en openen het kastje (of alle kastjes in haar huis synchroon) **automatisch gedurende 30 seconden de microfoon** terug naar alléén Edwin — **zonder dat zij bij een kastje hoeft te kunnen komen**.
4. Zelfs als zij 6 meter verderop in de gang of achter de badkamerdeur ligt en alleen zacht kan roepen of tegen een deur/vloer kan kloppen, hoort Edwin dat direct (en ziet hij bij meerdere kastjes zelfs op welke verdieping het geluid klinkt!) zodat hij meteen kan komen.
5. Na exact **30 seconden** sluit de microfoon gegarandeerd vanzelf met een sluittoon, keert het speaker-volume terug naar haar eigen ingestelde stand en gaat het rode lampje weer uit.

---

## 2. Vier Keiharde Privacy- & Beveiligingseisen (Vertrouwensbasis)

Omdat een microfoon op afstand kunnen openen extreem gevoelig ligt, gelden vier ononderhandelbare regels:

1. **Strikte Per-Persoon Toestemming voor Meerdere Naasten (Lokaal op Moeders Kastje!):**
   - Moeder kan met allerlei mensen in Kringen praten (buren, kennissen, andere familie). Voor **al die mensen** staat `PeerListenPermission` keihard op `LISTEN_PERM_DENIED` (de standaardwaarde). Hun oproepen kunnen haar microfoon **nooit** automatisch openen.
   - Specifiek door Moeder op haar eigen kastje aangewezen vertrouwenspersonen — bijvoorbeeld **zowel zoon (`#1 Edwin`) als dochter (`#4 Zusje`)** via `allow_listen = 1, 4` of `--allow-listen 1,4` — krijgen beiden lokaal `LISTEN_PERM_ALLOWED`.
   - **Strikte 1-op-1 exclusiviteit per oproep:** Ook wanneer meerdere kinderen op de toestemmingslijst staan, stuurt Moeders kastje de 30 seconden audio **uitsluitend** terug naar die éne gemachtigde persoon die op dát moment de Welzijnsoproep deed en de `ChallengeNonce` bevestigde (nooit tegelijk ongevraagd naar de andere gemachtigden of andere kringleden).
   - **Belangrijk:** De centrale Switchboard-server beslist hier **niet** over. Zelfs een gecompromitteerde server kan zichzelf of een derde nooit luisterrechten geven op Moeders kastje.
2. **Cryptografische Live-Bevestiging (Geen Replay-Aanvallen):**
   - Om te voorkomen dat een vastgelegd netwerkpakket uit het verleden opnieuw kan worden afgespeeld om de microfoon te openen, vereist het openen van het 30s-venster een **verse 64-bit `ChallengeNonce`** vanuit Moeders kastje die live door Edwins privésleutel bevestigd moet worden (1 netwerk-roundtrip van ~15 ms).
   - De terugkerende audio tijdens die 30 seconden wordt **uitsluitend** versleuteld en geadresseerd aan die éne geautoriseerde beller (nooit aan een hele groep).
3. **Harde 30-Seconden Begrenzing in Firmware (Geen "Oneindig Open"):**
   - Er bestaat in het protocol **géén** commando om een microfoon voor onbepaalde tijd open te zetten.
   - De toestandsmachine telt deterministisch maximaal `3000 ticks × 10 ms = 30,0 seconden` af (bewaakt met een `assert()`). Daarna valt de microfoon altijd dicht. Wil Edwin nog 30 seconden luisteren of geruststellen (*"Ik heb je gehoord Ma, ik kom er nú aan!"*), dan spreekt hij opnieuw in (met opnieuw een hoorbaar signaal in huis).
4. **Geen Stille Afluistermodus + Directe Lokale Afbreking:**
   - Het openen gaat **altijd** gepaard met een hoorbare aankondigingstoon én het felrode **MIC AAN**-lampje op het kastje.
   - Is er niets aan de hand en loopt Moeder wél langs het kastje? Één druk op een willekeurige knop op haar kastje breekt het luistervenster direct af (of laat haar gewoon antwoorden).

---

## 3. Cruciaal Akoestisch & DSP-Inzicht: Kloppen op een Deur op 6 Meter Afstand!

Hier zit een technisch detail waar standaard telefoons en slimme speakers vaak de mist in gaan:

1. **Standaard spraakfilters onderdrukken geklop en geschuifel!**
   - Moderne VoIP-systemen (en `libopus` wanneer het strak op `OPUS_SIGNAL_VOICE` of ruisonderdrukking staat) zijn getraind om **niet-stemgeluiden** — zoals bonken op een houten deur, tikken tegen een verwarmingsbuis of schuifelen over de vloer — weg te filteren als "stoorgeluid".
   - **Onze oplossing:** Zodra het kastje van Moeder het 30-seconden luistervenster ingaat (`CAPTURE_MODE_FAR_FIELD_CARE`), schakelen we `OpusEncoder` tijdelijk van `OPUS_SIGNAL_VOICE` naar **`OPUS_SIGNAL_AUTO`** zonder agressieve noise-gate. Daardoor komen ook doffe klappen op een deur, tikken of zacht kreunen natuurgetrouw door!
2. **Hoge Versterking Zónder Rondzingen (Waarom Half-Duplex hier Goud waard is):**
   - Tijdens de 30 seconden dat Moeders kastje luistert, staat Moeders eigen luidspreker **100% op mute** (en pas als Edwin zijn spreekknop weer indrukt om iets terug te zeggen, draait die richting tijdelijk weer om).
   - Omdat speaker en microfoon nooit tegelijk aanstaan, kunnen we tijdens het 30s-luistervenster de microfoon-voorversterking (**Far-Field AGC**) veilig **+20 tot +24 dB hoger** opendraaien om zacht geluid uit een andere kamer of achter een deur op te pikken — zonder énig risico op rondzingen, en zonder complexe Echo Cancellation nodig te hebben! Een snelle *soft-limiter* beschermt Edwins oren als er plotseling iets hards dicht bij de microfoon valt.

---

## 4. Impactanalyse per Systeemonderdeel

### 4.1 Impact op het `'UHUR'` Wire-Protocol (`include/core/wire_proto.h`)
Uitbreiding van `PacketKind` met vier controle-pakketten:

| Nieuw `PacketKind` | Richting | Betekenis & Inhoud |
|---|---|---|
| `PACKET_KIND_CARE_CALL_START` | Beller → Moeder | Start van een Welzijnsoproep (zoals `TALK_START`, maar verzoekt om een 30s luistervenster na afloop van het spreken + speelt bij Moeder een extra duidelijke oproeptoon). |
| `PACKET_KIND_LISTEN_CHALLENGE` | Moeder → Beller | Verzonden zodra Moeders kastje `TALK_END` ontvangt van een beller met `LISTEN_PERM_ALLOWED`. Bevat een verse 64-bit `ChallengeNonce`. |
| `PACKET_KIND_LISTEN_CONFIRM` | Beller → Moeder | Cryptografisch getekende/versleutelde bevestiging van `ChallengeNonce`. Pas na ontvangst hiervan gaat het 30s microfoonvenster daadwerkelijk open. |
| `PACKET_KIND_LISTEN_ABORT` | Beide kanten | Directe beëindiging van het 30s luistervenster (bijv. als Moeder op een knop tikt of als de beller klaar is met luisteren). |

### 4.2 Impact op `DuplexGate_Owned` (`include/core/duplex_gate.h`)
Uitbreiding van de 3 huidige toestanden (`LISTENING`, `TRANSMITTING`, `HANGOVER`) met één strak begrensde 4e toestand:

```text
                       (Beller spreekt: CARE_CALL_START .. TALK_END)
  ┌────────────────┐ ───────────────────────────────────────────────► ┌──────────────────────────┐
  │                │      + LISTEN_PERM_ALLOWED + Challenge OK        │ DUPLEX_STATE_            │
  │ DUPLEX_STATE_  │                                                  │ AUTO_REPLY_TX            │
  │ LISTENING      │ ◄─────────────────────────────────────────────── │ - Max 3000 ticks (30,0s) │
  │                │   Timer == 0  OF  Lokale knopdruk  OF  ABORT     │ - Lokale speaker MUTE    │
  └────────────────┘                                                  │ - Rode MIC-LED AAN       │
                                                                      │ - Far-Field AGC +24 dB   │
                                                                      │ - Alleen naar 1 beller!  │
                                                                      └──────────────────────────┘
```
- **Als Edwin tijdens die 30 seconden opnieuw op zijn PTT-knop drukt** (*"Ik hoor je Ma, blijf rustig liggen, ik ben over 5 minuten bij je!"*): het luistervenster pauzeert direct de microfoon en zet de speaker open zodat Moeder hem luid en duidelijk hoort, en hervat na zijn zin een vers 30s luistervenster (tenzij hij afsluit).

### 4.3 Impact op `IntercomCore_Owned` & Peer-Tabel (`include/core/intercom_core.h`)
- Toevoegen van Strong Type `PeerListenPermission` (`LISTEN_PERM_DENIED = 0`, `LISTEN_PERM_ALLOWED = 1`) aan elke `PeerInfo` entry.
- Standaardwaarde bij registreren van elke peer is altijd `LISTEN_PERM_DENIED`.
- Toevoegen van expliciete configuratiefunctie `IntercomCore_set_peer_listen_permission(...)` (gevoed vanuit lokale NVS-opslag op Moeders kastje).
- Tijdens `DUPLEX_STATE_AUTO_REPLY_TX` negeert de TX-router `selected_targets` en stuurt de audiopakketten **uitsluitend** naar de éne geverifieerde `authorized_listener` (`Edwin` of `Zusje`).

### 4.4 Impact op `OpusEncoder_Owned` & Audio-DSP (`include/core/opus_codec.h` + nieuwe `audio_dsp.h`)
- Nieuwe functie `OpusEncoder_set_capture_mode(OpusEncoder_Owned *ctx, CaptureMode mode)`:
  - `CAPTURE_MODE_NORMAL_VOICE` → `OPUS_SIGNAL_VOICE` + standaard spraak-AGC.
  - `CAPTURE_MODE_FAR_FIELD_CARE` → `OPUS_SIGNAL_AUTO` (behoudt kloppen/bonken/omgevingsgeluid) + verhoogde Far-Field AGC-versterking (`+24 dB`) met **ruisvloer-plafond (*max silence gain cap*)** en **soft-limiter**.
  - *Waarom het ruisvloer-plafond essentieel is bij meerdere kastjes in één huis:* als het kastje in de woonkamer stil is terwijl Moeder boven op de overloop roept, voorkomt het ruisvloer-plafond dat de lege woonkamer zijn eigen achtergrondruis met +24 dB opblaast en over het geluid van boven heen legt.
  - Deze `opus_encoder_ctl`-omschakeling werkt op de bestaande statische encoder-state met **0 bytes extra geheugen en 0 `malloc`**.
- **Nood-Volume Override (`PlaybackGainMode`):**
  - Normale oproepen volgen het door de bewoner gekozen woonkamer-/avondvolume (`GAIN_MODE_USER_LEVEL`).
  - Een geautoriseerde Welzijnsoproep dwingt tijdens de oproep tijdelijk `GAIN_MODE_EMERGENCY_FULL` af op de uitgangstrap/DAC, en herstelt automatisch `GAIN_MODE_USER_LEVEL` zodra de Welzijnsoproep eindigt.

### 4.5 Impact op Fysieke Hardware, Noodvolume & Multi-Kamer Opstelling (`MCP23017` / ESP32)
1. **Bij Edwin & Zusje op het kastje:**
   - Gewone druk op **[MA]** = normale intercom-oproep op Moeders ingestelde kamer-volume (microfoon bij Ma blijft na het spreken gewoon dicht; fijn voor dagelijkse gesprekken zoals *"Eet je mee?"*).
   - Druk op **[WELZIJNS-CHECK + MA]** (eigen knop of duidelijke twee-knops combinatie zodat het nooit per ongeluk gebeurt) = start *Roep & Luister* op **vol noodvolume** inclusief het 30s luistervenster.
2. **Hele-Huis Dekking met Satelliet-kastjes (Unique Selling Point):**
   - Bij Moeder thuis kunnen **twee of drie betaalbare kastjes** staan (bijv. *Woonkamer* beneden + *Overloop* boven bij slaapkamer en badkamer), gekoppeld onder één huishoud-groep bij Edwin en Zusje.
   - **Synchroon Roepen & Luisteren zonder rondzingen:** Bij een Welzijnsoproep roepen alle kastjes in Moeders huis tegelijk op vol volume. Zodra Edwin loslaat, muten **alle** speakers in haar huis tegelijk en openen **alle** microfoons synchroon voor 30 seconden — ze kunnen elkaar dus ook tussen verdiepingen nooit laten rondzingen!
   - **Verdiepings-herkenning bij de mantelzorger:** Omdat `IntercomCore` 4 gelijktijdige RX-streams ondersteunt en per zender energie bijhoudt, hoort Edwin beide verdiepingen gemixt én kan een indicator tonen uit welke ruimte (*Beneden* of *Boven*) het geluid of geklop komt.
3. **Bij Moeder op het kastje:**
   - **Rode "MIC AAN" LED:** Gekoppeld aan `(state == TRANSMITTING || state == AUTO_REPLY_TX)`. Er is dus fysiek geen verschil: zodra er geluid het huis verlaat, brandt het rode lampje altijd.
   - **Microfoon-plaatsing:** Een gevoelige omnidirectionele I2S MEMS-microfoon (zoals `ICS-43434` of `INMP441`) vrij gemonteerd aan de voorzijde van het kastje.
4. **Toestemming instellen bij plaatsing:**
   - In het lokale instelportaal van elk kastje bij Moeder zet je per gekoppeld familielid het vinkje *"Mag 30s luistervenster + noodvolume activeren"*, waarbij het opslaan bevestigd moet worden met een fysieke druk op de knop van het kastje zelf.

---

## 5. Waar past dit in onze Stapsgewijze Roadmap?

Het mooie is dat deze functie **géén Echo Cancellation (Fase 6)** nodig heeft omdat spreken en 30s luisteren elkaar netjes afwisselen! Daardoor kunnen we dit veel eerder inbouwen en testen:

1. **In Fase 2 (Audio-DSP + ESP32 Bring-up):**
   - Nemen we de **Far-Field AGC + Soft Limiter** en de `OpusEncoder_set_capture_mode` direct mee in `src/core/`, én kunnen we al op 6–8 meter afstand (en achter een dichte deur!) testen hoe goed geklop en zacht roepen via de I2S MEMS-microfoon van je ESP32-board doorkomen!
2. **In Fase 2b / Fase 4 (State Machine + Crypto Challenge):**
   - Bouwen we `PeerListenPermission`, `DUPLEX_STATE_AUTO_REPLY_TX` (de harde 30s timer + aankondigings-/sluittoon) en de `ChallengeNonce`-bevestiging in `src/core/`, volledig afgedekt met unit-tests die bewijzen dat:
     - Een ongeautoriseerde peer **nooit** de microfoon kan openen;
     - Een herhaald oud pakket (replay) wordt geweigerd;
     - De microfoon na exact `3000` ticks (`30,0 s`) gegarandeerd sluit.
