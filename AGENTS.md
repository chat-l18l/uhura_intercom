# AGENTS.md — Werkwijze & Strikte Embedded C Standaard

Dit document legt de verplichte werkwijze, architectuurprincipes en programmeerregels vast voor dit project (Linux Netwerk & Audio in C). Elke AI-agent en ontwikkelaar moet zich bij zowel ontwerp, implementatie als code-review **strikt en zonder uitzondering** aan deze regels houden.

---

## 1. Werkwijze (Design-First Workflow)

1. **Eerst Ontwerp, Dan Code:** Schrijf **nooit** direct implementatiecode voor nieuwe features of modules. Presenteer eerst een helder ontwerp met:
   - Systeem- en datastroom-architectuur (bijv. buffer-lifetimes, state machines, foutafhandeling).
   - Exacte data- en eigenaarschapstypes (`_Owned`, `_Loaned`, Strong Types, Parameter Structs).
   - Scheiding tussen pure domeinlogica en de Linux Hardware/OS-abstractielaag (HAL).
2. **Wacht op Goedkeuring:** Pas nadat het ontwerp expliciet is besproken en goedgekeurd, wordt overgegaan tot implementatie.
3. **Verificatie & Testbaarheid:** Omdat alle kernlogica losgekoppeld is van Linux-syscalls en audio-drivers, wordt kernlogica voorzien van deterministische unit-tests op Linux, gecompileerd met strikte compiler-flags (`-std=c11` of `-std=c23`, `-Wall -Wextra -Werror -Wpedantic -Wconversion -Wshadow`).

---

## 2. Coding Rules (NASA Power of 10 / Zenoh-pico Stijl)

Gedraag je als een senior embedded systems engineer. Pas aanvullend op onderstaande 7 kernregels ook de klassieke *NASA Power of 10* principes toe: geen recursie, begrensde lussen (bounded loops), minimale variabelenscope en beperkte/veilige preprocessor-macro's.

### Regel 1: Geen dynamische geheugenallocatie
- Gebruik **nooit** `malloc`, `calloc`, `realloc`, `free` (of `new`/`delete` in C++) na de opstart-/initialisatiefase — bij voorkeur helemaal niet.
- Gebruik vaste statische buffers, stack-allocatie van begrensde omvang, ringbuffers met vaste capaciteit of een vooraf gedefinieerde geheugen-`Arena` die bij opstarten eenmalig wordt ingericht.

### Regel 2: Strikte Ownership & Borrowing (Zenoh-pico stijl)
- **Naamgeving:** Maak in type-namen expliciet onderscheid tussen eigenaarschap (`_Owned`) en geleende referenties/views (`_Loaned`).
- **Levensduur van borrows:** Een `_Loaned` view mag **nooit** langer leven dan zijn `_Owned` bron en mag **nooit** worden opgeslagen in een langlevende context- of state-struct.
- **Move-semantiek in C:** Overdracht van eigenaarschap verloopt via een pointer naar de `_Owned` variabele (bijv. `AudioFrame_Owned *src`), waarbij de ontvangende of verplaatsende functie de bron **direct nul/ongeldig maakt** (`*src = (AudioFrame_Owned){0}`).
- Leen een object via een expliciete helperfunctie zoals `z_loan`-stijl (`AudioFrame_loan(const AudioFrame_Owned *self)`).

```c
typedef struct {
    uint8_t *data;
    size_t len;
    size_t capacity;
    bool is_valid;
} PacketBuffer_Owned;

typedef struct {
    const uint8_t *data;
    size_t len;
} ByteSlice_Loaned;

[[nodiscard]] ByteSlice_Loaned PacketBuffer_loan(const PacketBuffer_Owned *self);
void PacketBuffer_move(PacketBuffer_Owned * restrict dst, PacketBuffer_Owned * restrict src);
```

### Regel 3: Geen Naked Types
- Gebruik **nooit** ruwe primitieve types (`int`, `float`, `uint16_t`, `uint8_t *`, `size_t`) direct in functie-signaturen voor domeinwaarden of buffers.
- Verpak domeinwaarden altijd in **Strong Types** (`struct` met één veld of getypeerde `enum`) zodat de compiler eenheden en rollen afdwingt (bijv. `SampleRateHz`, `UdpPort`, `FrameCount`, `ChannelCount`, `GainDb`).
- Geef buffers nooit door als losse pointer; gebruik altijd een slice/span-struct inclusief lengte- en capaciteitsinformatie (`ByteSlice_Loaned`, `ByteSliceMut_Loaned`, `PcmSamples_Loaned`).

```c
typedef struct { uint32_t hz; } SampleRateHz;
typedef struct { uint16_t port; } UdpPort;
typedef struct { const int16_t *samples; size_t frame_count; } PcmMonoSlice_Loaned;
```

### Regel 4: Pure Functies & Expliciete State
- **Geen globale state:** Gebruik geen globale of functie-lokale `static` mutabele variabelen.
- **Expliciete context:** Geef alle benodigde state expliciet mee als eerste parameter via een getypeerde context-struct (bijv. `JitterBuffer_Owned *ctx`).
- Houd transformatie-, protocol- en DSP-functies zo **puur** mogelijk: deterministische output op basis van expliciete input en state, zonder verborgen neveneffecten.

### Regel 5: Defensief Programmeren
- **Assertions:** Dwing pre-condities, post-condities en invarianten overal af met `assert(...)`. Gebruik `_Static_assert(...)` / `static_assert(...)` om pakketgroottes, alignment, buffer-afmetingen en enum-grenzen tijdens het compileren te bewijzen.
- **Resultaatcontrole:** Markeer elke functie die een resource (`_Owned`) of foutcode/status (`Result` / `ErrCode`) teruggeeft met `[[nodiscard]]` (C23) of `__attribute__((warn_unused_result))` (GCC/Clang C11 via een centrale `NODISCARD` macro in één gedeelde basistypes-header).

```c
#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 202311L
  #define NODISCARD [[nodiscard]]
#elif defined(__GNUC__) || defined(__clang__)
  #define NODISCARD __attribute__((warn_unused_result))
#else
  #define NODISCARD
#endif
```

### Regel 6: Strikte Hardware- & OS-Abstractie (HAL)
- Houd alle domein-, protocol-, framing-, jitterbuffer- en audiobewerkingslogica **100% vrij van platform-specifieke headers** (geen `<sys/socket.h>`, `<arpa/inet.h>`, `<poll.h>`, `<alsa/asoundlib.h>`, `<pipewire/...>`, `<unistd.h>`, enz. in `core/`).
- Platform-specifieke Linux I/O leeft uitsluitend in een dunne adapterlaag (`platform/linux/`) aan de rand van het systeem.
- Hierdoor is 100% van de kernlogica deterministisch, zonder root/netwerk/geluidskaart, met gesimuleerde klok en buffers te testen.

### Regel 7: Parameter Object Pattern (Geen verwisselbare parameters)
- Geef een functie **nooit** twee of meer opeenvolgende parameters van hetzelfde type (voorkomt het per ongeluk omwisselen van argumenten).
- Verpak parameters in een specifieke parameter-struct (`...Params`) en roep de functie aan met **Designated Initializers**:

```c
typedef struct {
    SampleRateHz sample_rate;
    FrameCount   frames_per_packet;
    ChannelCount channels;
} StreamConfigParams;

NODISCARD StreamError Stream_init(StreamContext_Owned *ctx, StreamConfigParams params);

/* Aanroep: zelfdocumenterend en onmogelijk om te wisselen */
StreamError err = Stream_init(&stream_ctx, (StreamConfigParams){
    .sample_rate       = { .hz = 48000U },
    .frames_per_packet = { .count = 480U },
    .channels          = { .count = 2U },
});
```
- Alternatief bij exact 2 domeinwaarden: gebruik twee verschillende, niet-uitwisselbare Strong Types zodat de compiler verwisseling direct weigert.

---

## 3. Checklist voor Code Review (Zelfcontrole voor oplevering)

Controleer vóór elke oplevering van code elk punt:
- [ ] Is het ontwerp vooraf goedgekeurd door de gebruiker?
- [ ] Nul aanroepen naar `malloc`, `calloc`, `realloc` of `free`?
- [ ] Elke struct met resources of referenties expliciet gemarkeerd met `_Owned` of `_Loaned`?
- [ ] Geen enkele `_Loaned` referentie opgeslagen in een langlevende struct?
- [ ] Wordt bij elke move de bron direct op `{0}` / ongeldig gezet?
- [ ] Geen naked types (`int`, `float`, `uint8_t*`) voor domeinwaarden of buffers in functie-parameters?
- [ ] Geen twee opeenvolgende parameters van hetzelfde type (Parameter Object Pattern / Strong Types gebruikt)?
- [ ] Geen globale variabelen; alle state expliciet via context-structs?
- [ ] `assert()` op pre-/post-condities, `_Static_assert()` op kritieke groottes, en `NODISCARD` op alle foutcodes/resources?
- [ ] Bevat `core/` nul Linux-, netwerk- of audio-systeemheaders?
