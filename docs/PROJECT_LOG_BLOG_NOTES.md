# Engineering Log & Achtergrondverhalen (`hexnut.net` / *Elektor*)

Dit document bundelt de inhoudelijke verhaallijnen, praktijkscenario's, ontwerpafwegingen en het werkproces achter **UHURA**, bedoeld als rijke broncontext voor artikelen op **hexnut.net**, **Elektor Magazine** en onderwijsmateriaal bij **Fontys ICT**.

De focus ligt bewust **niet** op losse code-commits, maar op *welke menselijke en fysische problemen we tegenkomen*, *welke ontwerpfouten op de loer liggen*, en *hoe we die met doordachte systeemarchitectuur oplossen*.

---

## Aflevering / Thema 1: Waarom een "Dom" Kastje Slimmer is dan een Smartphone

### Het menselijke vertrekpunt
Wie voor een ouder op leeftijd of slechthorende familielid een communicatie-oplossing zoekt, merkt al snel dat consumentenelektronica de verkeerde kant op is geoptimaliseerd:
- Smartphones, tablets en pc's vragen om updates, ontgrendelcodes, touchscreens en menu's, en doen er een minuut over om op te starten.
- Het volume van een telefoon op luidsprekerstand is vaak te zacht of te schel voor iemand met ouderdomsslechthorendheid (*presbyacusis*).
- Bestaande intercoms werken óf alleen binnen één huis (DECT/analoog), óf dwingen iedereen in één centrale cloud-app van een fabrikant.

### De ontwerpfilosofie ("Star Trek Enterprise" aan de muur)
In *Star Trek* opent niemand een app om met de machinekamer te praten: je drukt op één fysieke knop aan de muur, roept *"Brug aan Machinekamer"*, en laat los. Vandaar de projectnaam **UHURA** (en de 4-byte protocol-magic `'UHUR'`).

Vertaald naar echte elektronica en software leverde dat vier harde producteisen op:
1. **Koude start binnen ~2 seconden:** Een ESP32 met 8 MB PSRAM en I2S-audio start direct op vanuit zijn lokale flash-cache, zonder eerst op een zware cloud-synchronisatie te hoeven wachten.
2. **1 Knop = 1 Persoon, 1 Lampje = 1 Betekenis:** Uitgevoerd met een eenvoudige `MCP23017` I2C GPIO-expander. Geen knipperpatronen die je uit het hoofd moet leren: groen is bereikbaar, geel/wit betekent *deze persoon spreekt nú*, en één felrood lampje betekent *jouw microfoon staat open*.
3. **Asymmetrische "Kringen" in plaats van één groepschat:** In het echte leven wil *Jij* kunnen praten met *Moeder* en *Zus*, en wil *Zus* óók kunnen praten met haar *Schoonmoeder* — zonder dat die schoonmoeder automatisch bij jou in de woonkamer op een knop verschijnt. Door één kastje lid te laten zijn van meerdere onafhankelijke *Kringen* (elk met een simpele koppelcode) ontstaat een natuurlijk sociaal netwerk zonder ingewikkeld beheer.
4. **Van huiskamer tot Eduroam:** Thuis prikt het kastje via *UDP Hole Punching* rechtstreeks door de router heen (0 ms omweg), maar in een afgeschermd universitair lab op **Eduroam** (waar *Symmetric NAT* en UDP-blokkades gelden) schakelt het automatisch terug naar een eind-tot-eind versleutelde tunnel over poort `443`.

---

## Aflevering / Thema 2: Het Val-Scenario — "Roep & 30 Seconden Luisteren"

### Het probleem waar draagbare techniek faalt
Ouderen die alleen wonen zijn vaak bang om te vallen en dan niemand te kunnen bereiken:
- De mobiele telefoon of vaste hoorn ligt op tafel in de woonkamer terwijl je in de gang uitglijdt.
- Een **smartwatch** met valdetectie ligt 's nachts op het nachtkastje aan de lader, en gaat juist vóór het douchen in de badkamer (de gevaarlijkste plek met natte tegels!) af.
- Zelfs een traditionele alarmketting om de hals wordt in bed of onder de douche vaak afgedaan, of iemand raakt versuft en drukt niet op de knop.

Het idee kwam rechtstreeks van moeders zelf: *"Kun jij vanuit jouw huis niet gewoon door de intercom roepen 'Ma, is alles goed daar?' — en dat het kastje mij dan 30 seconden kan horen zónder dat ik naar de knop hoef te kruipen? Al lig ik achter de deur van de gang, dan kan ik nog roepen of met mijn vuist tegen de deur slaan zodat jij weet dat je moet komen."*

### De technische en ethische puzzel: Veiligheid zónder Afluister-risico
Een microfoon die van buitenaf geactiveerd kan worden raakt direct aan privacy en veiligheid. Als je dat slordig ontwerpt, bouw je onbedoeld een afluisterapparaat. Daarom hebben we dit op **systeem- en protocolniveau** dichtgetimmerd met vijf bewuste ontwerpafwegingen:

1. **Waarom een keiharde limiet van 30 seconden in de firmware?**
   - We hebben bewust besloten om **géén commando "zet microfoon open tot nader order"** in het `'UHUR'`-protocol op te nemen. Als een netwerkverbinding wegvalt tijdens een gesprek, zou een microfoon anders urenlang open kunnen blijven staan.
   - In plaats daarvan telt een begrensde toestandsmachine (`DUPLEX_STATE_AUTO_REPLY_TX`) in de pure C-kern exact `3000 ticks × 10 ms = 30,0 seconden` af, bewaakt met compile-time en runtime assertions. Na 30 seconden klapt de microfoon **altijd** fysiek dicht met een hoorbare sluittoon. Wil de mantelzorger nog iets vragen of geruststellen (*"Ik heb je gehoord, ik stap nu in de auto!"*), dan roept hij opnieuw in.
2. **Lokale toestemming op het kastje van Moeder (niet in de cloud!):**
   - Moeder kan prima een praatje maken met de buurvrouw of kennissen via de intercom, maar die krijgen standaard `LISTEN_PERM_DENIED`.
   - Uitsluitend de cryptografische identiteit van specifiek door haar aangewezen personen (alleen zoon en dochter, bevestigd met een fysieke knopdruk op háár kastje) krijgt lokaal `LISTEN_PERM_ALLOWED`. Zelfs als de centrale adresboek-server ooit gehackt zou worden, kan die server niemand luisterrechten geven.
3. **Protocol-uitbreiding tegen Replay-Aanvallen (`ChallengeNonce`):**
   - Wat als iemand op het lokale Wi-Fi of internet vandaag een geldig "Welzijnsoproep"-pakket van de zoon onderschept en dat pakket morgen opnieuw naar Moeders kastje stuurt?
   - Daarom opent `PACKET_KIND_CARE_CALL_START` de microfoon **niet** direct. Zodra de zoon uitgesproken is, stuurt Moeders kastje eerst een eenmalig willekeurig 64-bit getal (`PACKET_KIND_LISTEN_CHALLENGE`). Pas wanneer het kastje van de zoon binnen 2 seconden live bewijst dat hij die specifieke uitdaging kent (`PACKET_KIND_LISTEN_CONFIRM`), gaat het 30-seconden venster open — en wordt de audio uitsluitend naar die éne beller teruggestuurd. Een oud pakket herhalen wordt direct geweigerd.
4. **Nood-Volume Override & Geen Stille Modus:**
   - Staat het volume bij Moeder 's avonds op 10% of op stil, dan drukt een geautoriseerde Welzijnsoproep dat tijdelijk door naar **100% noodvolume**, zodat de oproep door het hele huis klinkt (en zet het volume na afloop netjes terug).
   - Er bestaat géén "stille afluisterstand": het kastje speelt altijd eerst een duidelijke aankondigingstoon en zet het felrode **MIC AAN**-lampje aan. Eén tik op een knop bij Moeder breekt het luistervenster bovendien direct af.

---

## Aflevering / Thema 3: Drie Verborgen Akoestische Valkuilen (en hun Oplossing)

Bij het doorrekenen van het scenario *"iemand ligt 6 meter verderop achter een deur en klopt tegen het hout"* kwamen drie prachtige audio-engineering details naar boven:

1. **Waarom standaard VoIP-codecs geklop op een deur uitwissen!**
   - Spraakcodecs (ook `libopus` wanneer die op `OPUS_SIGNAL_VOICE` staat, en al helemaal moderne AI noise-suppression op telefoons) zijn getraind om **niet-stemgeluiden weg te filteren als stoorgeluid**. Een doffe bonk op een badkamerdeur of geschuifel op de gang wordt door een telefoon vaak vakkundig weggepoetst!
   - **Oplossing:** Zodra UHURA het 30-seconden luistervenster opent, schakelt de encoder dynamisch over van `OPUS_SIGNAL_VOICE` naar `OPUS_AUTO` zonder ruisonderdrukking, gekoppeld aan een vaste-komma **Far-Field AGC (+24 dB)** met een zachte limiter. Zo wordt een zachte klop op 6 meter afstand 16× versterkt zonder dat een harde klap dichtbij pijnlijk clipt.
2. **De 120 ms Voor-Toon truc (geen echo van je eigen pieptoon):**
   - Als je op Moeders speaker een *"Microfoon gaat nu open"*-toon afspeelt op exact hetzelfde moment dat haar `+24 dB` microfoon opengaat, brult die pieptoon vol in haar eigen gevoelige microfoon terug naar de beller.
   - Daarom houdt `DuplexGate` in de eerste `12 ticks` (`120 ms`) van het luistervenster de microfoon nog dicht terwijl de aankondigingstoon speelt, dempt op `tick 13` de speaker naar 0%, en zet dán pas de `+24 dB` microfoon 30 seconden open. Omdat speaker en microfoon elkaar nooit overlappen, kan het systeem zelfs bij maximale versterking **nooit rondzingen** — zonder dat er zware Echo Cancellation nodig is!
3. **Hele-Huis Dekking met Meerdere Kastjes & het *Noise-Floor Plafond*:**
   - Omdat de kern van UHURA tot 4 gelijktijdige inkomende audiostreams kan decoderen en mixen, kun je bij een oudere twee of drie kastjes plaatsen (bijv. *Woonkamer* beneden + *Overloop* boven bij slaapkamer en badkamer). Bij één Welzijnsoproep roepen ze synchroon door het hele huis en luisteren ze daarna allebei 30 seconden terug.
   - **De valkuil:** Ligt iemand boven op de overloop en is de woonkamer beneden doodstil, dan zou een naïeve +24 dB AGC in de lege woonkamer de eigen achtergrondruis 16× opblazen en over het hulpgeroep van boven heen leggen.
   - **De oplossing:** Een *Noise-Floor Gain Cap* in onze vaste-komma DSP begrenst de versterking in een doodstille ruimte op maximaal `+6 dB`, en laat de volle `+24 dB` pas los zodra een signaal boven de stiltevloer uitkomt. Bonus: op het kastje van de mantelzorger licht direct de indicator `(GELUID!)` op bij *Boven* — zodat je onderweg erheen al weet op welke verdieping iemand ligt.

---

## Aflevering / Thema 4: Software Engineering in het Tijdperk van Agentic AI

Hoe bouw je een embedded C23 audiosysteem met dit niveau van betrouwbaarheid in uren in plaats van maanden met behulp van AI-agents (*Google Gemini* / *Claude Opus*)?

Niet door "vibe coding" (*"bouw een intercom voor me"*), maar door **strenger te engineeren dan ooit**:

1. **Regels vóór Code (`AGENTS.md`):** Voor er één `.c`-bestand bestond, zijn 7 keiharde embedded-regels vastgelegd (*NASA Power of 10* + *Zenoh-pico* eigenaarschap): nul `malloc`/`free` (zelfs `libopus` draait op statische buffers), strikt onderscheid tussen `_Owned` en `_Loaned` geheugen, geen *naked* primitieve types (`int`, `uint8_t*`) maar Strong Types (`StationId`, `ChallengeNonce`, `VolumePercent`), nul globale variabelen, en 100% scheiding tussen pure logica (`src/core/`) en hardware (`src/platform/`).
2. **Eerst Ontwerp & Kritisch Doorvragen (`docs/DESIGN*.md`):** Elke nieuwe functie (zoals het 30s luistervenster of de ESP32 PSRAM-verdeling waarbij I2S-DMA in interne SRAM móét blijven om haperen tijdens flash-writes te voorkomen) wordt eerst als ontwerpdocument uitgewerkt, besproken en goedgekeurd.
3. **Bewijs door Deterministische Simulatie (`pixi run test` in 0,03 seconde):** Omdat `src/core/` geen enkele socket of geluidskaart aanraakt, bootst `tests/test_care_listen.c` volledige huishoudens na (*Edwin*, *Zus*, *Ma-Beneden op 0% nachtstand*, *Ma-Boven met zacht geklop*, en een *ongeautoriseerde Buurman*), inclusief een onderschept netwerkpakket dat opnieuw wordt afgespeeld (*Replay Attack*). Elke wijziging compileert in minder dan 2 seconden onder `-Wall -Wextra -Werror -Wconversion` en bewijst wiskundig dat alle veiligheidsregels houden.

---

## Aflevering / Thema 5: Een Native Windows 11 Intercom van 800 KB — Zónder Electron, .NET of DLL-Hell

### Waarom een Windows `.exe` naast Linux en ESP32?
Bij Moeder in de woonkamer en op de overloop komt straks het fysieke **ESP32-kastje** met echte drukknoppen en LEDs. Maar waar zitten haar zoon en haar dochter overdag tijdens het werk of thuis? Vaak gewoon achter een **Windows 11 pc of laptop**.

Als zij vanaf hun bureau direct bereikbaar willen zijn (of even een Welzijnsoproep willen doen), wil je je zus niet vragen om Linux, Python, Java of een 250 MB zware Electron/WebView-applicatie te installeren. Je wilt een mapje met **één enkel klein bestandje `uhura.exe`** + een tekstbestandje `uhura.txt` kunnen opsturen: uitpakken, dubbelklikken en binnen `0,05 seconde` staat het "virtuele intercomkastje" klaar op haar scherm.

### Het bewijs van een schone Hardware-Abstractielaag (HAL)
Hier betaalde onze strikte scheiding uit Aflevering 4 zich op spectaculaire wijze terug:
- Aan de volledige `src/core/` (Opus-codec, `'UHUR'`-protocol, `ChallengeNonce` beveiliging, Far-Field AGC, jitterbuffers en mixer) hoefde **letterlijk nul regels code** te worden aangepast voor Windows.
- Door in `pixi.toml` de MinGW-w64 UCRT C23 cross-compiler (`gcc_win-64`) op te nemen, rolt er vanuit dezelfde Linux/WSL-omgeving met één commando (`pixi run build-win`) een volledig statisch gelinkte `uhura.exe` (~800 KB inclusief `libopus` met SSE/AVX2 optimalisaties!) uit.
- **Nul externe `.dll`-bestanden:** een inspectie met `objdump` laat zien dat `uhura.exe` uitsluitend standaard Windows systeem-DLL's gebruikt (`USER32`, `GDI32`, `WINMM`, `WS2_32` en `UCRT`), die op elke Windows 10/11 pc al in `C:\Windows\System32` staan.
- Zelfs alle 7 unit-testsuites compileerden in één keer naar `.exe` en slaagden direct native op Windows 11.

### Drie praktische details die het voor familie afmaken
1. **Echte vasthoud-knoppen met de muis:** In een terminal is "ingedrukt houden" lastig, maar in een puur Win32 GDI-venster (`WM_LBUTTONDOWN` + `WM_LBUTTONUP`) werkt de muis exact zoals de fysieke drukknop op het ESP32-kastje: muisknop ingedrukt houden = spreken (rode balk licht op), loslaten = luisteren.
2. **Dubbelklik-configuratie (`uhura.txt`):** Geen gedoe met snelkoppelingen of command-line parameters voor familieleden; `uhura.exe` zoekt naast zichzelf naar `uhura.txt` voor naam, poort en bekende posten.
3. **Meerdere gemachtigde mantelzorgers (`allow_listen = 1, 4`) met 1-op-1 privacy:** Op het kastje van Moeder kunnen zowel *broer* (`#1`) als *zus* (`#4`) op de toestemmingslijst staan om in geval van zorg het 30-seconden luistervenster te activeren (terwijl een buurman of kennis dat recht niet heeft). Daarbij bewaakt de kern een subtiele maar belangrijke privacyregel: zodra *zus* een Welzijnsoproep doet, stuurt Moeders kastje die 30 seconden audio **uitsluitend naar zus** — en nooit tegelijk ongevraagd naar broer of anderen.
