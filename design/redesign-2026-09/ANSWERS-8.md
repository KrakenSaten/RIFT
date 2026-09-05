# Svar på del 8 — begrunnet, i handoverens rekkefølge

Tall for farger er regnet etter RGB565-kvantisering (WCAG-kontrast, OKLab-avstand), ikke bedømt.

## 8.1 NODES: er rekkeviddeskalaen svaret?

**Form A beholdes; ingen kolonne C og ingen bånd B. Påliteligheten bæres på samme rad, i cellens form.**

Spørsmålet var om «hvor pålitelig når jeg dem» får plass på raden med «hvor langt». Det gjør det, fordi vi har tre former som overlever sollys og bare bruker to: fylt, hul, og fylt-med-prikk (drawRect + 4×4 senter). Cellene under hoppantallet tegnes **fulle** når ruten er bekreftet (brukt i en levering med ACK, eller path-hash for en pakke vi faktisk mottok siste 30 min) og som **prikk** når tallet bare stammer fra en advert eller en eldre hash. Hul bortenfor, som før. Ukjent rute er fortsatt ti hule og `?`.

Hvorfor ikke B (konsentriske bånd): båndene er anonyme. NODES' element er et navn som må leses, og et bånd kan ikke bære 14 navn på 170 px uten å bli en liste igjen. Hvorfor ikke C (ekstra kolonne): raden er full (navn 120, celler 120, hopp 24, alder 24 = 288 + marger + tommel). En ellevte kolonne ville kostet navnetegn, og det er navnet brukeren leter etter. Formen koster null bredde.

Kostnad: én ekstra boolsk per node (`route_confirmed`), som firmwaren allerede har implisitt i ACK-håndteringen.

## 8.2 Hjem-skjermens komposisjon

**Én skjerm, ett spørsmål — men to ting sto der uten å svare på det.**

Radaranimasjonen viste ikke data; den viste at skjermen tegnet. Den er byttet med en aktivitetsstripe (20 celler, ett minutt hver, fylt = hørt noe), som er det bokstavelige svaret på «er meshet levende der jeg står» og koster 20 fyll per minutt i stedet for fyll per bilde. TROPO får egen rad og erstatter ikke radioparameterne; en rad som skifter mening er en modus brukeren må lære. Knapperaden er ikke et andre spørsmål: DISCOVER og ADVERT er verbene til det samme spørsmålet (er det noen der? spør). De blir, men flyttes til y 180 med noten på y 200, så de 14 px nærmest nav-linjen er tomme. To størrelse-3-elementer på én grunnlinje beholdes; det er en linje, ikke to konkurrenter.

## 8.3 Dag-grønn mot kanalfarge 1 og 2

**Begge deler: «levert» får en form, og dagmodusens `ok` flyttes.**

Form først, fordi den løser problemet uavhengig av farge: leveringsmerket er `√ N hops` (0xFB) i ok. Ingen kanal- eller avsendernavn kan begynne med `√`, og merket står i høyre slot der navn aldri står.

Farge i tillegg, fordi ACTIVE på hjem og `√` i COMMS ikke bør ligge 0,053 fra en kanalfarge når et flytt er gratis. `ok` er en statusfarge som ikke trenger å fungere på svart i dagmodus, så den kan gå mørkere enn kanalbåndet:

| Kandidat | RGB565 | RGB | på hvitt | på svart | OKLab til kanal 1 | til kanal 2 | til aksent |
|---|---|---|---|---|---|---|---|
| dagens #428610 | 0x4422 | 66,134,16 | 4,52 | 4,65 | 0,061 | 0,053 | 0,323 |
| **ny #2A6400** | **0x2B20** | **41,101,0** | **7,10** | 2,96 | **0,124** | **0,104** | 0,360 |

Avstanden til det nærmeste kanalparet er mer enn doblet og ligger over dobbelt av gulvet 0,047. Kontrasten på hvitt går fra 4,52 til 7,10. Kontrasten på svart er irrelevant for en dagverdi (regel 12: grønn skifter allerede verdi mellom modusene). Fargen leses fortsatt som grønn (OKLab-kulør 137°). Nattgrønn #39C800 er uendret.

## 8.4 Aksentens sju betydninger

**Fra sju til fire.** Tre kunne bæres av form eller av `rule`:

| Betydning | Nå | Hvorfor |
|---|---|---|
| Aktiv fane | accent, uendret | streken er formen, fargen støtter |
| Valgt / handlingsbar | accent, uendret | den viktigste; alt annet skal peke mot den |
| Varsel (NO SIGNAL, batteri ≤15, overvåket, `no ack`, tap-ramme, armert) | accent, uendret | én kategori, ikke fire |
| Egen melding | **2 px strek i fg** | ingen annen rad har en strek; formen er unik uten farge |
| Ulest | **3×3 kvadrat i fg** | fravær/tilstedeværelse er form; sifre er avgjort mot |
| Overleggsramme | **2 px rule** | rammen sier «oppå» ved å være en boks; fg ville konkurrert med innholdet, accent sa «handle» om hele boksen |
| Ordmerkets 2 px | accent, uendret | brand, ikke betydning |

Fotlinjens `ENTER: ...` var ikke listet men var aksent på NODES; nå er alle tastehint `mid` (og `on_accent` inne i valgt rad). Aksent i tekst forekommer dermed bare som varsel.

## 8.5 Prosa på SYSTEM

**Ja, det finnes en visuell behandling, og scope-teksten overlever ikke.**

Advarsel = ramme i accent (varsel) rundt 1–3 linjer i fg, første linje `! CANNOT BE UNDONE`. Forklaring = én linje i mid, uten ramme, aldri mer. Skillet er form (ramme) og lengde, ikke gråtone. De fire tap-advarslene får rammen; KEEP er forvalgt så et feiltolket tapp ikke fyrer. Scope-skjermens fem linjer er redusert til én (`Scope limits a channel to one region.`) og resten går til dokumentasjonen: det er lærestoff, ikke en beslutning brukeren tar i felt.

## 8.6 Lister større enn skjermen

**Ett vindusmønster, beskrevet i 00-SYSTEM §4 og brukt av alle ni listene.** Kort: heltall rader i viewport; markøren trekker vinduet; tommel på x 318–319 (spor rule 1 px, tommel fg 2 px, min 6 px) bare når `total > view`; antall i overskriftslinjen; ingen `+N more`; inne i overlegg ved boks.x+w−5. Overvåkningslisten i RADAR og oppdagelsespanelet er lagt inn i det. COMMS-historikken bruker samme tommel med pikselverdier.

## 8.7 Navnefarge 0–3 er kanalfargene

**Uendret.** Kollisjonen er reell men ikke tvetydig: en farge står aldri alene. Kanalfargen står som ramme rundt kanalens navn øverst; navnefargen står på avsenderens navn ved x 40. Leseren har alltid teksten. Å bytte fire av tolv til andre verdier i det 1 091 store båndet vil senke settets minste innbyrdes avstand (tolv var der utvidelse sluttet å kjøpe noe, og fire nye må presses inn i de samme kulørgapene). Å flytte kolliderende hash-slots per kanal gir samme person ulik farge i ulike kanaler, som bryter det navnefargen er til for.

## 8.8 En femte kanal

**«Ingen farge» er riktig, og fraværet får en form: ramme i `rule`.** Kanal 0 (Public) rammes allerede slik. Kanal 5+ rammes likt: rule sier «har identitet, ikke farge». I samtalelisten får kanal 5+ en 2 px brikke i rule, kanal 0 ingen. Ingen aksent, ingen lysstyrkekode, ingen påstand om en identitet som ikke finnes.

## 8.9 To rullemodeller

**Uendret, med felles overflate.** Stepping i lister er riktig fordi en rad er berøringsmålet og markøren må lande på en. Pikselrulling i COMMS er riktig fordi blokkene er 1–6 linjer; stepping per blokk ville hoppet inntil 72 px og pikselrulling i lister ville gitt halve rader. Det brukeren møter er nå likt: samme 6 px-regel for tapp, samme tommel på samme x, samme «ingen rundbryting». Inkonsistensen ligger i motoren, ikke i det som vises.

## 8.10 Ordmerket

**Uendret, begge instanser.** Klipping langs diagonalen koster ~80 fyll per instans; på oppstart er det gratis, på hjem er det på timer (ordmerket ligger i en skjerm som tegnes hvert 700 ms, og skjøten må tegnes på nytt etter hvert statusord). To ordmerker som ser ulikt ut er verre enn ett som er litt grovere: ordmerket er det eneste elementet som skal se identisk ut overalt. Bitmap-ruten står fortsatt åpen for oppstart alene når noen vil betale 2 688 skrivinger én gang, men da gjelder samme innvending.

## 8.11 Emoji

**Ingen nye emoji-glyfer.** To nye glyfer foreslås i stedet, og de er ikke emoji: ø (0xED) og Ø (0xE8), fordi CP437 mangler dem og hver norsk melding trenger dem (`Lillestrøm`, `Tyri-Åsen`-problemet i nodes-night er dette). Forkastelsesregel for fremtidige glyfer: en 5×7-glyf uten farge skal gjenkjennes i 1× på armlengde av to av tre testere uten forhåndsvisning; gjenkjennes den ikke, tegnes blokken (0xDB), som er ærlig om at den ikke kan tegne tingen. Sol, hjerte, pil, note består i dag; ansikter gjør det ikke og forblir blokker.

## Funn utenfor del 8 (verdt å melde)

- nodes-night 5. sept viser `Tyri¦Åsen`: UTF-8 tegnes rått. Transliteringstabell i 00-SYSTEM §9.
- `HOPS` (x 266) og `HEARD` (x 290) overlapper med 4 px og HEARD går til x 320. Rettet med `AGE` høyrestilt til 314.
- SYSTEM viste `PAGE 1/2` i nav-slotten der alle andre viser batteri. Med én liste bortfaller det.
- `LINK -80 / -4` på hjem var eneste tall uten etikett. Nå `RSSI -80  SNR -4`.
