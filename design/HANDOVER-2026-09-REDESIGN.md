# RIFT — handover til design, redesignrunde september 2026

Gjelder alle skjermene i RIFT (`KrakenSaten/RIFT`, gren `rift-tdeck`, firmware 0.9.2
pluss fikser til og med 5. september 2026). Dokumentet er selvstendig: en designer
trenger ikke tilgang til repoet for å arbeide fra det. Der dette dokumentet og et
skjermbilde er uenige, er skjermbildet riktig, og avviket er verdt å melde.

Forrige handover (`HANDOVER-2026-08.md`) gjaldt én runde med tre skjermer. Denne ber
om et **helhetlig redesign** av alle fem skjermene og de fem overleggene, innenfor
rammene i del 2 og reglene i del 7. Det som er fast, er fast fordi panelet krever det,
ikke fordi noen foretrekker det.

---

## 1. Hva RIFT er, og hva vi ber om

RIFT er et brukergrensesnitt for LilyGO T-Deck: en håndholdt LoRa-mesh-radio med
fysisk tastatur, trackball, kapasitiv berøringsskjerm og et 320×240-panel. Den
brukes utendørs, ofte i sollys, av og til med hansker, og spørsmålet brukeren har er
«virker radioen, og hvem når jeg?». Det er en feltterminal, ikke en telefon-app.

Fem skjermer nås fra en navigasjonslinje nederst, hver med ett spørsmål:

| Skjerm | Spørsmålet den svarer på |
|---|---|
| RIFT (hjem) | Er meshet levende der jeg står? |
| NODES | Hvem er der ute, og hvor langt unna? |
| RADAR | Hvilke Wi-Fi- og BLE-enheter er rundt meg? |
| COMMS | Samtaler |
| SYSTEM | Innstillinger, og hva gjør enheten akkurat nå? |

**Vi ber om** en gjennomgang av alle fem, med et helhetlig grep: felles rytme, felles
chrome, konsekvent bruk av aksentfargen, og svar på de åpne spørsmålene i del 8.
Skjermene er bygget på ulike tidspunkt, chromet er endret under dem, og
interaksjonsmodellen ble til etter at fire av dem var ferdige. Drift mellom dem er
sannsynlig og er nettopp det et friskt blikk fanger.

**Vi ber ikke om** en ny visuell identitet. Ordmerket, aksentfargen `#FF4100`,
CP437-fonten og natt/dag-palettene er avgjort og målt. Del 9 lister det som er
prøvd og forkastet, med begrunnelse, så en runde ikke bruker seg selv på det.

---

## 2. Panelet bestemmer mer enn smak

Hver begrensning under er fysisk. Ingen kan forhandles bort ved å foretrekke noe annet.

| Begrensning | Verdi | Hva den forbyr |
|---|---|---|
| Oppløsning | 320 × 240, liggende | Ingen andre kolonne med prosa. En full bredde-liste holder ca. 16 rader |
| Fargedybde | RGB565 (5-6-5 bit) | Farger må velges **etter** kvantisering, ikke før |
| Font | 6 × 8 CP437 bitmap, kun heltallsskalering | Ingen vekter, ingen kursiv, ingen bokstavavstand, ingen kerning. Størrelse 2 er nøyaktig dobbelt |
| Radavstand | 12 px | 8 px glyf, 4 px luft. Tettere er uleselig på armlengde |
| Antialiasing | Ingen | En diagonal er en trapp. En 1 px-detalj lander på en piksel eller finnes ikke |
| Klipping | Ikke i driveren, men se 2.1 | Ingen klipperektangel. Effekten fås ved å tegne og så male over |
| Primitiver | `fillRect`, `drawRect`, `drawXbm`, tekst | Ingen linje, sirkel, bue, gradient eller alfa |
| Diagonaler | Satt sammen av `fillRect`-løp | Mulig og i bruk, men hver diagonal er håndstegt |
| Bitmaps | `drawXbm`: 1-bits maske, én farge, per piksel | Et 96×28-merke er 2 688 pikselskrivinger. Greit på en skjerm som tegnes én gang, for dyrt på en som tegnes på timer |
| Tegnebudsjett | Deler SPI-bussen med LoRa-radioen | En skjerm som tegnes hvert 700 ms må være billig. Blokkeres bussen sultes radioen, og det finnes ingen vaktbikkje som fanger det |
| Tegnsett | CP437 | Ingen `…`, `—` eller `−`. Æ, Ø og Å finnes ikke i den engelske delen; skjermtekst er engelsk. `·` er 0xFA |

### 2.1 Klipping, slik den faktisk finnes

COMMS tegner det rullende innholdet inkludert det som faller utenfor, maler bakgrunn
over alt over og under visningen, og tegner chromet på nytt oppå. Det er reell
klipping for områder avgrenset av **vannrette kanter**. Det koster to fyll i full
bredde og en chrome-omtegning per bilde. Ikke-rektangulær klipping er mulig i
prinsippet (ca. 80 fyll per diagonal per bilde) men ikke bygget.

### 2.2 Sollys er designmiljøet

Under reflektert dagslys forsvinner forskjellen mellom to gråtoner helt, mens
forskjellen mellom et fylt og et hult kvadrat ikke gjør det. Overalt der RIFT kunne
kodet en verdi i lysstyrke, koder den i form i stedet. Dette er den regelen som har
avgjort flest valg.

### 2.3 Inndata, målt

- **Tastaturet** har verken tasterepetisjon eller taste-opp. Et langt trykk kan ikke
  oppdages. Alt som trenger en andre gest må bygges av diskrete trykk; nordisk-velgeren
  bruker dobbelttrykk på en vokal.
- **SYM-laget** gir symbolene som er trykket på tastene. Det er ikke en ledig tast.
- **Trackballen** er fire retninger pluss klikk. Klikk er Enter. Dobbeltklikk er
  forrige skjerm. Det er den eneste inndata som virker med hansker.
- **Berøring** poller hvert 8 ms, kalibrert 5. september: hjørnene mapper til (2,0) og
  (319,0). Et tapp rapporteres ved slipp. Et slipp etter mer enn 6 px reise er et drag,
  ikke et tapp, uansett hva skjermen gjorde med bevegelsen.

---

## 3. Palett

To moduser, samme geometri, byttet fargetabell. Ingenting flytter seg når brukeren
bytter.

### 3.1 Natt (standard)

| Rolle | Hex | RGB565 | Bruk |
|---|---|---|---|
| `bg` | `#000000` | `0x0000` | Felt |
| `bar` | `#1A1A1A` | `0x18C3` | Chrome-bånd |
| `fg` | `#FFFFFF` | `0xFFFF` | Primærtekst |
| `mid` | `#9A9A9A` | `0x9CD3` | Sekundærtekst, chrome, etiketter |
| `dim` | `#8A8A8A` | `0x8C51` | Nedtonet tekst |
| `rule` | `#707070` | `0x738E` | Hårstreker og rammer |
| `accent` | `#FF4100` | `0xFA00` | Merke, aktiv tilstand, varsel |
| `on_accent` | `#000000` | `0x0000` | Blekk inne i et aksentfyll, 6,01:1 |
| `ok` | `#39C800` | `0x3E40` | Levert, bekreftet |

### 3.2 Dag

| Rolle | Hex | RGB565 | Merknad |
|---|---|---|---|
| `bg` | `#FFFFFF` | `0xFFFF` | |
| `bar` | `#EFEBEF` | `0xEF5D` | |
| `fg` | `#000000` | `0x0000` | |
| `mid` | `#5A5A5A` | `0x5ACB` | |
| `dim` | `#6B6B6B` | `0x6B4D` | **Mørkere enn `mid`, ikke lysere** |
| `rule` | `#8C8C8C` | `0x8C51` | |
| `accent` | `#FF4100` | `0xFA00` | Samme verdi, annen rolle |
| `accent_txt` | `#202020` | `0x2104` | Aksenten er uleselig som tekst på hvitt |
| `on_accent` | `#000000` | `0x0000` | Samme som natt: aksenten skifter ikke, så blekket gjør det heller ikke |
| `ok` | `#428610` | `0x4422` | Ikke nattgrønn. 4,52:1 på hvitt |

### 3.3 Reglene paletten koder

1. **Ingenting mørkere enn `#6E6E6E` på svart overlever en 6×8-glyf uten
   antialiasing.** Empirisk. Derfor finnes ingen fjerde grå.
2. **Nedtoning inverterer med feltet.** På hvitt må dim også være mørkere. En lysere
   grå på hvitt leses som deaktivert, ikke som sekundær.
3. **Aksenten har én verdi og to roller, og kontrast er symmetrisk.** `#FF4100` er
   6,0:1 mot svart og 3,5:1 mot hvitt. På svart kan den være tekst. På hvitt kan den
   bare være fyll, og **aldri grunn under hvit tekst**: hvitt på aksenten er samme
   3,5:1. Blekket i et aksentfyll er `on_accent`, svart, i begge moduser. Tretten
   flater gjorde dette feil fram til 0.9.2.
4. **Regn kontrast, ikke bedøm den.** Tre reelle feil var usynlige for øyet og
   åpenbare for aritmetikken. `tools/palette-check.py` i repoet skriver ut alle
   tallene i dette dokumentet, så de kan etterprøves.

### 3.4 Kanalfarger og navnefarger

En kanals identitetsfarge kan ikke bytte mellom modusene, så én verdi må klare 4,5:1
mot både svart og hvitt. Det binder luminansen til et smalt bånd (L 0,175–0,183).
Av 65 536 RGB565-verdier klarer 1 091 det, alle like mørke, ulike bare i kulør.
«En lys og en mørk» er aritmetisk utilgjengelig.

Fire kanalfarger, kulør i OKLab:

| Plass | RGB565 | RGB | Kulør | På svart | På hvitt |
|---|---|---|---|---|---|
| 1 | `0x73E0` | `rgb(115,125,0)` | 115° | 4,66 | 4,50 |
| 2 | `0x0429` | `rgb(0,134,74)` | 154° | 4,51 | 4,66 |
| 3 | `0x631E` | `rgb(99,97,247)` | 278° | 4,58 | 4,58 |
| 4 | `0xD170` | `rgb(214,44,132)` | 355° | 4,55 | 4,61 |

Plass 0 (den offentlige kanalen alle har) får ingen farge. En femte kanal får heller
ingen: en gjentatt farge er verre enn en fraværende, fordi den påstår en identitet
som er usann.

**Tolv navnefarger** for avsendere i COMMS, tildelt ved hash av navnet, så samme
person har samme farge på alle enheter og etter omstart. De fire første er
kanalfargene. Målt i OKLab er hver av de tolv lenger fra aksenten og de to grønne enn
settets egen dårligste indre avstand (0,047), som regnes som gulvet for lesbarhet i
6 px. Tolv er der utvidelse slutter å kjøpe noe; den fulle tabellen står i
`RiftLogic.h` og i `DESIGN-REVIEW-2026-09.md` §2.4.

**Det ene målte problemet som står åpent:** dagmodusens `ok`-grønn er OKLab 0,053
fra kanalfarge 2 og 0,061 fra kanalfarge 1. Det er det nærmeste paret i hele
paletten. I dagmodus er et kanalnavn og «levert»-merket nesten samme farge. Se 8.3.

---

## 4. Chrome og rutenett

**Navigasjonslinjen er det eneste faste chromet.** 14 px nederst, y 226–239.

- Hårstrek ved y = 226, etiketter ved y = 228
- Fem faner: `RIFT · NODES · RADAR · COMMS · SYSTEM`, sentrert ved x = 20, 81, 145,
  209, 270. Ikke jevne 64 px-kolonner, som ville klippet SYSTEM mot kanten
- Aktiv fane: 2 px aksentstrek under **og** fargeskift. Streken er nå dimensjonert
  til etiketten (bredde pluss 8 px), ikke til kolonnen. Streken finnes fordi
  gråsteget mellom aktiv og inaktiv forsvinner i sollys
- `RIFT`-fanen får merkefargen bare mens den er aktiv. I dagmodus tegnes den som en
  aksentbrikke med svart tekst
- Nederst til høyre: batteriprosent i `mid`, aksent ved ≤ 15 %. På SYSTEM står
  sidetallet der i stedet
- Ulest-prikk (3×3 aksent) ved x 240 når noe er ulest et sted

**Kroppen er y 0–226. Det er ingen tittellinje.** Den ble fjernet fordi den sa det
navigasjonslinjen allerede sa, og kostet én melding COMMS-historikk.

**Overskrifter er per skjerm og betingede.** En skjerm tegner én linje ved y = 2 i
`mid` bare når den har noe å si som layouten ikke kan: hvilket steg i en flyt, hvilken
kontakt en melding går til, hvor mange noder som er hørt.

**Radavstand er 12 px overalt.** Cellen er 6×8, så en rad i full bredde er 53 tegn.
Det ene unntaket er repeater-panelets CLI-utskrift på 10 px, som er rullebuffer og
ikke layout.

---

## 5. Interaksjonsmodell

Tre inndata, alle aktive samtidig, ingen dominerende. **Ingenting er kun berøring.**
Trackballen er garantien, berøring er bekvemmeligheten.

### 5.1 Drag

Lister ruller radvis: NODES, RADAR, SYSTEM og samtalelisten flytter markøren ett
steg per 16 px fingerreise, med resten båret mellom prøvene. COMMS ruller pikselvis,
fordi blokkene varierer fra én til seks linjer. Drag bryter ikke rundt; piltastene
gjør det.

Et slipp er et tapp bare hvis fingeren reiste 6 px eller mindre. Regelen ble
strammet 4. september etter at et langt drag som ikke flyttet noe ble levert som tapp
og aktiverte «Delete channel» på SYSTEM.

### 5.2 Treffprøving

Rader treffprøves mot **der de faktisk landet**, registrert under tegning, ikke mot en
beregnet avstand. Seksjonsoverskrifter tar en rad hver og flytter alt under.

### 5.3 Overlegg

Fem flater tegnes over skjermen under i stedet for å erstatte den: meldingsforhåndsvisning,
navngi overvåket enhet, repeater-panel, nordisk tegnvelger og oppdagelsesresultat.
Skjermen under beholder tilstanden sin og leveres tilbake urørt. Overlegg tegnes som
en innrammet boks inne fra kantene, ramme i aksent. Oppdagelsespanelet er
x 6, y 22, w 308, h 186.

### 5.4 Bekreftelse

Destruktive repeater-kommandoer armeres på første Enter og fyrer på andre. Armert
tilstand holdes som **radindeksen**, ikke som en boolsk, så å flytte markøren og
trykke Enter ikke kan fyre den kommandoen du bekreftet.

---

## 6. Skjerminventar, slik det er i dag

### RIFT (hjem)

Overskrift i størrelse 3: `ACTIVE` / `IDLE` / `QUIET` / `NO SIGNAL`, i grønt / hvitt /
grått / aksent. `meshcore.io` over i `mid`. Ordmerket øverst til høyre. Tre datarader
under: alder på siste mottak, pakketall, USB/BLE-lenke. En liten radaranimasjon til
høyre for midten. **En knapperad ved y = 198**, 14 px høy: `DISCOVER 0-HOP`,
`ADVERT NEAR`, `ADVERT MESH`. Valgt knapp er fylt i aksent med svart tekst, de andre er
rammet i `rule`. En notelinje ved y = 214 forklarer den valgte. Under en tropo-åpning
erstattes radioparameterlinjen av `TROPO OPEN · peak N hops`.

### NODES

Fem bøtter øverst, `DIRECT | 1-2 | 3-5 | 6+ | NO ROUTE`, ved x 2, 65, 128, 191, 254
med søyler på inntil 58 px som sammenlignes med hverandre. Kolonneoverskrifter ved
y 56: `NODE`, `REACH`, `HOPS`, `HEARD`. Liste fra y 68, rullbar, valgt rad er
aksentfyll med svart blekk. Hver rad: ferskhetsmerke (fylt = hørt siste 30 min, hul =
eldre), navn på 120 px, **en rekkeviddeskala på ti celler** fra x 140 med 12 px
avstand, fylt opp til hoppantallet og hul bortenfor, eksakt tall i `HOPS`, relativ
alder i `HEARD`. Ukjent rute tegner ti hule celler og `?`. Den valgte raden utvider
seg med ruten gjennom de repeaterne den faktisk gikk, og en handlingslinje:
`ENTER: message` / `ENTER: control` / `ENTER: read`.

### RADAR

Et stort tall, så tre signalbånd `CLOSE` / `MID` / `FAR` med sine dBm-områder, tegnet
som rader av 6×8-celler, én per enhet, fylt for nær og middels, hul for fjern. En rad
som renner over tegner en aksentcelle. Under: liste sortert sterkest først, med
markør. Enter veksler et fossefall over kanalbelegg over tid. `W` er fossefall, `S`
bytter kilde (Wi-Fi / BLE / begge). Enheter kan navngis og overvåkes; en overvåket
enhet som dukker opp gir en lampe i aksent øverst med svart tekst. Overvåkningslisten
har plass til 12, men bare seks rader får plass på skjermen. Se 8.6.

### COMMS

Én samtale om gangen. Fanestripe med fire kanaler øverst (kanalfargen som ramme, aktiv
fane som aksentfyll med svart tekst), historikk lagt ut nedenfra fra skrivelinjen, og
skrivelinje med tegntelling nederst. Egne meldinger har en 2 px aksentstrek langs
venstre kant. Hver rad: klokkeslett ved x 4 i `dim`, avsendernavn fra x 40 i
avsenderens farge, og høyre slot med leveringstilstand eller hoppantall i ord.

**Samtalelisten** (Enter på tom skrivelinje) holder 96 oppføringer: kanalene først,
så samtaler med historikk, så kontakter hørt nylig. Hver rad: ulest-prikk (svart når
raden er valgt), 2 px kanalbrikke eller avsenderfarge, navn fra x 11 på 240 px, og til
høyre `ROOM` der det er en romserver pluss relativ alder (`3m`, `4d`, `>99d`, `?` når
klokken ikke er satt). Valgt rad er aksentfyll. Overskriften sier `N conversations`.

### SYSTEM

To sider. Side 1: handlinger venstre for delelinjen (node-navn, legg til / slett kanal,
scope per kanal, path-hash-størrelse, dag/natt, sett klokke), lesbare diagnoserader til
høyre. Side 2: grupper `DEVICE`, `MESH`, `RUNTIME`, `EVENT LOG` med rader som NODE,
KEYBOARD, TOUCH (mappet og rå), GPS, CLOCK, CONTACTS, PATH CACHE, TROPO, FREE HEAP,
EXT POWER, MSG WAKE, LAST RESET, **USB SERIAL** (companion / RESCUE CLI) og **BOOT
BTN** (rå nivå på GPIO0), begge nye 4. september, samt BOOT og SLOWEST med
oppstartstider. Nederst de to nyeste logglinjene og `ENTER: open log`. En 128-linjers
hendelseslogg og en 96-linjers RX/TX-logg åpnes herfra. Tett, og hver rad har funnet
en maskinvarefeil. Svaret er organisering, ikke sletting.

### Overlegg: repeater-kontroll

Fra NODES. Fire moduser: visning, passord, kommando, meny. Viser repeaterens
statistikk (oppetid, lufttid, duty cycle, batteri, pakketellere) på 12 px-rutenett,
telemetri i Cayenne LPP med enheter, og en CLI-utskrift på 10 px nederst. En kommando
velges fra en meny i stedet for å skrives. Hemmeligheter skjules i 30 sekunder.
Valgt menyrad er aksentfyll med svart tekst.

### Overlegg: oppdagelse

`DISCOVER 0-HOP` samler repeatere i direkte rekkevidde. To SNR-kolonner er poenget:
`rx` er hvor godt vi hørte svaret, `tx` er SNR-en repeateren målte på vår
forespørsel. Ingenting annet i firmwaren kan vise den andre.

### Overlegg: meldingsforhåndsvisning, navngi enhet, nordisk velger

Forhåndsvisningen lister de seks nyeste med avsender og tid; Enter åpner COMMS.
Nordisk velger reises ved dobbelttrykk på en vokal og viser variantene som celler,
valgt celle i aksentfyll.

### SYSTEM-underskjerm: kanal-scope

Liste over kanaler mot region, `(node default)` der ingen er satt, valgt rad i
aksentfyll. Inntastingsskjermen har fire linjer forklarende prosa. Se 8.5.

### Oppstart

Ordmerket ved x 32, y 78 i `fg`, skjøten fra venstre kant til midten, stroppelinjene
`R A D I O  I N T E L L I G E N C E` og `&  F I E L D  T E R M I N A L` under.
Statuslinje bare når SPIFFS faktisk formateres.

---

## 7. Regler som skal overleve et redesign

Hver er lært av en feil som ble sendt ut.

1. **Regn kontrast, ikke bedøm den.** Verifiser etter RGB565-kvantisering.
2. **Form overlever sollys; lysstyrke gjør det ikke.**
3. **Aksenten er fyll på hvitt og tekst på svart, og blekket i et fyll er svart.**
4. **Finn aldri på data for å fylle en layout.** Ukjent hoppantall, tvetydige
   rutehasher og tomme lister skal se ut som det de er. `?` er en gyldig glyf.
5. **Hvert logisk element skal kunne nås og ses.** Kan en layout skjule et element,
   må valget trekke det inn i bildet.
6. **Én skjerm, ett spørsmål.**
7. **En tom tilstand skal si hvorfor den er tom.** «No adverts heard since boot» er
   en annen påstand enn «no adverts heard».
8. **Aksenten er opptatt.** Den betyr allerede: aktiv fane, egen melding, her kan du
   handle, varsel, ulest, overleggsramme, armert bekreftelse. Hver ny bruk svekker de
   andre. Se 8.4.
9. **Hvert berøringsmål skal ha en tastaturvei.**
10. **En fiks på én skjerm skal gjøres på søsknene.** Treffprøving og drag landet på
    noen skjermer og ikke andre, to ganger, og begge gangene fant en bruker den
    manglende.
11. **Alle skjermstrenger er CP437 og engelske.** `·` skrives som 0xFA.
12. **Statusgrønn skifter verdi mellom modusene** (`#39C800` natt, `#428610` dag).

---

## 8. Åpne spørsmål, i rekkefølge etter hvor mye et godt svar endrer

### 8.1 NODES: er rekkeviddeskalaen svaret, eller første halvdel av det?

Form A fra septemberrunden er bygget: ti celler per rad. Form B (konsentriske bånd) og
C (A pluss en kolonne) står åpne. Skjermen ruller nå ved drag, så en form høyere enn
158 px er ikke lenger diskvalifisert. RADARs fylt/hul-celle er et bevist mønster her,
men RADARs elementer er anonyme og NODES' har navn som må kunne leses. Det gjenstående
spørsmålet er om «hvor pålitelig når jeg dem» kan bæres på samme rad som «hvor langt».

### 8.2 Hjem-skjermens komposisjon

To elementer i størrelse 3 deler én grunnlinje, og nederste tredjedel har fått tre
knapper, en notelinje og en tropo-linje som kan erstatte radioparameterne. Er det
fortsatt én skjerm med ett spørsmål, eller to?

### 8.3 Dag-grønn mot kanalfarge 1 og 2

OKLab 0,053 og 0,061, nærmere enn noe par av navnefarger. `ok` er en statusfarge, så å
flytte den er en avgjørelse, ikke en korreksjon. Alternativene: flytte `ok` i dagmodus
innenfor 4,5:1 på hvitt og bort fra de fire, eller gi «levert» en form i tillegg til
farge. Tallene står i `tools/palette-check.py`.

### 8.4 Aksentens sju betydninger

Hvilke av dem bør være noe annet? Kandidater med minst kostnad: ulest-prikken (kunne
vært form alene), overleggsrammen (kunne vært `rule` med tykkere strek).

### 8.5 Prosa på SYSTEM

Fire advarsler mot uopprettelig tap må stå. Scope-skjermen la til fem linjer av den
forklarende sorten. Finnes det en visuell behandling som skiller en tap-advarsel fra en
forklaring, og overlever scope-teksten regelen eller skal den til dokumentasjonen?

### 8.6 Lister som er større enn skjermen

Overvåkningslisten i RADAR holder 12 og viser 6; rader 7–12 tegnes over fotlinjen og
navigasjonslinjen. Oppdagelsespanelet holder 16 og viser 11 uten «+N more». Samtalelisten
holder 96 og har vindu. Vi ønsker ett vindusmønster som alle lister med markør bruker.

### 8.7 Navnefarge 0–3 er kanalfargene

En person hvis navn hasher til 0 tegnes i kanal 1s farge, som også står på fanestripen.
Kollisjon som betyr noe, gitt at navnet står ved siden av fargen? Eller skal de fire
første navnefargene være fire andre av de 1 091 som kvalifiserer?

### 8.8 En femte kanal

Fire farger er grensen for kanalidentitet. En femte får ingenting. Er «ingen farge»
riktig, eller skal den få en ikke-farge-markør, gitt at aksenten er opptatt og lysstyrke
er utilgjengelig?

### 8.9 To rullemodeller på én enhet

Lister stepper; COMMS ruller pikselvis. Begge er valgt med grunn, men brukeren beveger
seg mellom dem hele tiden. Koster inkonsistensen mer enn tilpasningen gir?

### 8.10 Ordmerket

Det tegnes som `RIFT` i størrelse 3 med en 4 px skjøt og 2 px aksent i, langs en
diagonal på 12/78. Skjæringen langs skjøten ble forkastet fordi klipping ikke fantes;
det gjør den nå for vannrette kanter og i prinsippet for diagonaler (ca. 80 fyll per
instans). Verdt det på oppstartsskjermen alene, og godta at de to instansene skiller
seg? Bitmap-ruten er fortsatt tilgjengelig med kjente kostnader.

### 8.11 Emoji

CP437 har ingen. Et lite sett er mappet til erstatninger (`:)`, `+1`, hjerter, piler,
sol, note); resten er blokker. Gjenstående vei er håndtegnede 6×8-glyfer, og da er
forkastelseslisten like viktig som glyfene: en ugjenkjennelig glyf er verre enn en
blokk, fordi blokken innrømmer at den ikke kan tegne tingen.

---

## 9. Avgjort mot. Ikke foreslå på nytt uten ny informasjon

Hver er prøvd eller kostnadsberegnet. Grunnen står, så et argument mot grunnen er mulig.

- **Gradient, glød eller alfa.** Ingen blanding i driveren
- **Lysstyrke eller opasitet som datakoding.** Forsvinner i sollys
- **En femte grå.** Ingenting mørkere enn `#6E6E6E` overlever på svart
- **En lys kanalfarge sammen med en mørk.** Aritmetisk utilgjengelig
- **Aksent som tekst på hvitt, og hvit tekst på aksent.** 3,5:1 begge veier
- **Lysere dim i dagmodus.** Leses som deaktivert
- **Tittellinje tilbake.** Sa det navigasjonslinjen sa, kostet en melding historikk
- **Meldingsforhåndsvisning i samtalelisten.** En rad er én 12 px-linje
- **Ulest-tall som sifre.** Uleselig i det rommet, og feil spørsmål
- **Permanent aksentbrikke på RIFT-fanen.** Leses som valgt fra alle skjermer
- **Høyrestilte egne meldinger i COMMS.** Koster halve bredden per linje
- **Tilfeldige navnefarger per økt.** Noen som skifter farge ved omstart er verre enn
  ingen farge
- **Rundbryting under drag.** Leses som at listen hoppet
- **Berøring-eneste kontroll.** Trackballen er hanske-garantien
- **NODES 1a og 1b, SYSTEM 1e, ordmerke 2a og 2b, 16×16 med skjøt.** Se
  `HANDOVER-2026-08.md`

---

## 10. Hva vi ønsker levert

1. **Én spesifikasjon per skjerm og overlegg**, i enhetskoordinater 320×240, med
   begge moduser, i formen `rift-nodes-system-spec.md` bruker: koordinater, tilstander
   (tom, ukjent, valgt, full, feil), strenger i CP437, og hvilken palettrolle hver
   flate bruker. Nye farger bare med kontrasttall etter kvantisering, mot begge felt.
2. **En rendering per skjerm i 2×** (640×480 PNG) som referanse, med samme forbehold
   som før: mål fra spesifikasjonen, ikke fra pikslene.
3. **En endringsliste**: hva som ble endret fra dagens skjerm, og hvorfor, i samme
   form som del 8 her, så beslutningene overlever til neste runde.
4. **Svar på del 8**, eller en begrunnet «uendret».
5. **Ett vindusmønster for lister** (8.6), beskrevet én gang og brukt av alle.

Format for selve designarbeidet står fritt. Forrige runde brukte `.dc.html` i
Claude Design; PNG pluss markdown er like bra så lenge koordinatene er reelle.

---

## 11. Skjermbilder

**Alle bildene i `design/screens/` er fra 20. august og foreldet.** De viser ikke
COMMS-redesignet, avsenderfarger, knapperaden på hjem, rekkeviddeskalaen i NODES,
repeater-panelet, oppdagelsespanelet, scope-skjermen eller de nye SYSTEM-radene.

Firmwaren har ingen skjermdump. Ferske bilder er fotografier av enheten, og det er
verdt å ta dem i både natt- og dagmodus, innendørs og i sol. Et alternativ som kan
bygges på en time er en USB-kommando som strømmer rammebufferen (150 KB RGB565) til
PC-en som PNG; si fra om det er ønsket før runden starter, så blir bildene eksakte.

---

## 12. Filer

| Fil | Innhold |
|---|---|
| `design/DESIGN-REVIEW-2026-09.md` | Forrige runde, engelsk, med full navnefargetabell og hva som ble gjort |
| `design/HANDOVER-2026-08.md` | Augustrunden: hva som ble avgjort og forkastet |
| `design/rift-nodes-system-spec.md` | Eksempel på spesifikasjonsformen vi ønsker |
| `design/channel-colours.md` | Utledningen av de fire kanalfargene |
| `design/comms-redesign.md` | Retningen for COMMS, gjennomført |
| `design/DESIGN-HANDOFF.md`, `design/handoff.md` | Opprinnelig konsept og første håndoff |
| `tools/palette-check.py` | Skriver ut alle kontrast- og avstandstall i dette dokumentet |
| `REVIEW-2026-09-03.md` | Kodegjennomgangen som gjorde touch, drag og aksenten ferdig målt |

Firmware: `KrakenSaten/RIFT`, gren `rift-tdeck`, versjon 0.9.2 pluss commits til og
med 5. september 2026. Enheten som skal fotograferes kjører nøyaktig denne.
