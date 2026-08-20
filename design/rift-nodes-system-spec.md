# RIFT — spesifikasjon for valgte skjermer

Gjelder valgte varianter fra `RIFT NODES + SYSTEM skisser.dc.html`: **1c** (NODES) og
**1d** (SYSTEM). Alle koordinater er enhetspiksler, 320×240. Radavstand `RIFT_LINE_H` = 12,
tegncelle 6×8, `setTextSize(1)` overalt der ikke annet står. Chrome er uendret: navbar
y 226..239, ingen tittelbar.

Fargeroller er de samme som i `handoff.md`. Aksent `#FF4100` er tekst på svart og fyll med
reversert tekst på hvitt.

**Glyffer.** Alle strenger her er CP437. Ingen `…` (bruk tre ASCII-punkt), ingen `—` og ingen
`−` (bruk `-`). Midtpunktet `·` finnes i tabellen på **0xFA** og må skrives som det kodepunktet
i C-strengen, ikke som UTF-8 — ellers tegnes to urelaterte blokker. Æ, Ø og Å finnes ikke i den
engelske delen av tabellen; skjermtekstene står derfor på engelsk.

---

## NODES (1c) — bøtter, liste, ruten utvider valgt rad

Skjermen svarer på fire spørsmål i rekkefølge: hvor stort er nettet, hvor spredt er det,
hvem er aktiv, og hvordan kom den valgte fram.

### Overskrift — y 2..9

- `n HEARD` i `mid` ved x=2. Ingen fyll.
- `MAX n HOPS` høyrejustert til x=318 i `mid`. Utelates når ingen rute er kjent.

### Bøttebånd — y 14..46

Fire kolonner, 79px bredde, venstrekant x = 2 / 81 / 160 / 239.

| Rad | y | Innhold |
|---|---|---|
| Etikett | 14 | `DIRECT` `1-2` `3-5` `6+` i `mid` |
| Søyle | 26 | `fillRect(x, 26, w, 6)` i `fg` |
| Tall | 36 | antall i `fg`, `setTextSize(1)` |

Søylebredde: `w = round(count / maxCount * 64)`, minimum 2 når `count > 0`, ingen søyle når
`count == 0`. `maxCount` er den største av de fire — søylene er en intern sammenligning, ikke
en absolutt skala.

Intervallene er faste. De skal **ikke** tilpasses nettet; en kolonne som betyr noe annet fra
gang til gang betyr ingenting.

Noder uten kjent hoppantall havner ikke i noen bøtte. Summen av de fire kan derfor være mindre
enn `n HEARD`, og det er riktig — `?` i listen er stedet de finnes.

### Rule — y=52, full bredde, `rule`

### Kolonneoverskrift — y 56..63, i `mid`

`NODE` ved x=14 · `HOPS` høyrejustert x=278 · `HEARD` høyrejustert x=318.

### Liste — fra y=68, 12px pitch

Per rad:

- Friskhetsmarkør ved x=2, 5×5: `fillRect` = hørt under 30 min, `drawRect` = eldre. Begge i `fg`.
  Form, ikke gråtone.
- Navn ved x=14, kuttet ved 20 tegn (120px) slik at det ikke kolliderer med `HOPS`.
- Hoppantall høyrejustert x=278 i `fg`, eller `?` i `mid` når ruten er ukjent eller tvetydig.
- Alder høyrejustert x=318 i `fg`, relativ: `2m` `4h` `3d`. Aldri absolutt i listen — den er
  fire tegn bred, og `14:32` sier ingenting om hvor lenge siden det er uten at brukeren regner.

**Aldersformat, avgjort.** Relativ i listen, absolutt i detaljraden når klokka er satt:
`heard 12m ago · 14:32`. Relativ svarer på spørsmålet listen stiller (hvem er aktiv nå);
absolutt er det du trenger når du skal fortelle noen andre når noden ble hørt. Når klokka ikke
er satt faller detaljraden tilbake til bare `heard 12m ago` — ingen `--:--`, ingen oppdiktet tid.
Grenser: under 60 s `now`, deretter `nm`, over 90 min `nh`, over 24 h `nd`, over 9 d `9d+`
(fire tegn er taket).

Antall rader: 13 når ingen rad er utvidet.

### Valgt rad

- `fillRect(0, y, 320, 12)` i aksent, tekst reversert i hvitt. Markøren tegnes hvit inni fyllet.
  Samme merking i begge moduser — fyll, ikke tekstfarge.
- Rett under følger to detaljrader i `mid`, 12px pitch:
  - `via RPT-NORD > ? > RPT-7 > ...` ved x=14. Tre ASCII-punkt når ruten er lengre enn raden.
  - `REPEATER · LAST HOP -88 · 14:32` ved x=14, og `ENTER: message` høyrejustert x=318 (aksenttekst
    på natt, `fillRect(240, y, 78, 12)` med hvit tekst på dag). Klokkeslettet utelates når klokka
    ikke er satt.
- Listen under skyves 24px ned. Rullingen må holde valgt rad **og** de to detaljradene innenfor
  y ≤ 225; hvis ikke, rull til valgt rad ligger 24px over nedre kant.

### Ukjent og tvetydig

- Ukjent rute: `?` i `HOPS`, og detaljraden sier `flood, route unknown` — ikke et tall.
- Tvetydig hoppbyte: `?` i ruten, og detaljraden sier `2 candidates, not resolved`.
  Aldri første treff.

### Tom liste

`No adverts heard since boot` ved (2, 68) i `mid`. `since boot` er en del av påstanden.

### Rekkevidde for inndata

Trackball opp/ned flytter valg én rad og ruller listen. Hver logisk node må kunne velges;
et valg som ikke er tegnet finnes ikke. Berøring treffer radene der de faktisk er tegnet —
registrer radhøydene ved render, som `_tabs_y` gjør i COMMS.

---

## SYSTEM (1d) — to sider

Venstre/høyre på trackball bytter side. Navbarens høyre slakk viser `1/2` i stedet for
batteriprosent mens SYSTEM er aktiv — prosenten står på de fire andre skjermene.

### Side 1 — handlinger

- `ACTIONS` ved (2, 2) i `mid`, `PAGE 1/2 - RIGHT: READINGS` høyrejustert x=318 i `mid`.
- Rule y=16.
- Menyrader fra y=20, 12px pitch, full bredde. Handling ved x=2 i `fg`, tilstand høyrejustert
  x=318 i `mid`.
- Valgt rad: `fillRect(0, y, 320, 12)` i aksent, hvit tekst x=2.
- Under valgt rad, når handlingen har en konsekvens som ikke kan angres, én forklaringsrad i
  `mid` ved x=2. Disse fire skal stå:
  - `Send advert (neighbours)` -> `direct RF only - use whole mesh before a first DM`
  - `Delete channel` -> `the key is lost with it`
  - `Add channel` -> `#hashtag channels are not secret`
  - nøkkelvisning -> `write it down - not shown again`
- Bunn: rule y=196. `KEY EXPORT` / `DISABLED` y=202 (`mid` / `ok`).
  `up/down select, ENTER activates` y=214 i `dim`, versjon høyrejustert samme rad.

Radene er de ti som finnes i koden i dag; ingen fjernes.

### Side 2 — avlesninger

Delelinje x=160, y 2..225, 1px `rule`.

Grupper med overskrift i `mid` og 1px `rule` under, deretter etikett/verdi-rader 12px pitch:
etikett i `mid` venstrejustert, verdi i `fg` høyrejustert i sin kolonne.

| Kolonne | Gruppe | Rader |
|---|---|---|
| Venstre x=2 | `DEVICE` | NODE, KEYBOARD, LAST KEY, I2C BUS, TOUCH, GPS, CLOCK |
| Venstre x=2 | `MESH` | PATH CACHE, HOPS, MSGLOG, phases |
| Høyre x=166 | `RUNTIME` | FREE HEAP, EXT POWER, MSG WAKE, LAST RESET, BOOT, SLOWEST |
| Høyre x=166 | `EVENT LOG` | antall nye / lagret, to siste linjer i `dim`, `ENTER: open log` i aksent |

25 tegn per kolonnerad. Statusfarge bare der det er en tilstand: `KEYBOARD ok` i `ok`,
`GPS not found` i aksent.

Høyre kolonne er den som vokser når boot-tider legges til. Nye rader legges i `RUNTIME`;
når gruppen går forbi y=225 er det gruppen som skal rulle, ikke skjermen som skal krympe.

---

---

## Ordmerket (2c) — uten bitmap-asset

Skjøten kan tegnes med fonten som allerede finnes, så ingen hånd-pikslet bitmap er nødvendig.
Skjøtelinjen er

```
seamY(x) = 92 - (x - 30) * 12 / 78        // ca. -8.8° , y 97 ved x=0 og y 48 ved x=319
```

Rekkefølge, alt inn i `GFXcanvas16` før blitting:

1. `setTextSize(3)`, `fg`, skriv `RIFT` ved (34, 76) — den nedre halvdelen.
2. Blank alt **over** skjøten: for hver `x` i 34..106, `fillRect(x, 0, 1, seamY(x))` i `bg`.
3. Skriv `RIFT` igjen ved (37, 73) — den øvre halvdelen, forskjøvet 3px opp og til høyre.
4. Blank alt **under** skjøten i samme x-område: `fillRect(x, seamY(x), 1, 240 - seamY(x))` i `bg`
   — men bare der trinn 3 tegnet, dvs. y < 97. I praksis `fillRect(x, seamY(x), 1, 24)`.
5. `drawLine(0, 96, 319, 47)` i aksent, to piksler tykk (tegn linjen to ganger, andre gang
   med y+1). Den går kant til kant og krysser ingen bokstavstamme.

Kostnad: to tekstkall og ~146 én-piksel-rektangler. Rekkefølgen er viktig — blankingen i trinn 2
må skje før trinn 3, ellers spises den øvre halvdelen.

Denne oppskriften gjelder splash. Navbaren beholder `RIFT` som ren tekst: ved 12px høyde blir
R-en 5×7 og skjøten forsvinner i to piksler. 16×16-marken (3b) er tegnet for de stedene der det
finnes 16px, og må da hånd-piksles — der er det ingen vei rundt.

---

## Marken — bitmaps

Pikslet ut fra skissene og lest tilbake piksel for piksel. Bytes er MSB først, klare for
`drawBitmap(x, y, bitmap, w, h, colour)`.

**16×16 klarer ikke skjøten.** Den 2px brede streken spiser R-ens nedre venstre ben, og det som
står igjen er en form som ser bestemt ut uten å være det. På 16px er marken derfor R alene.

### 16×16 — R alene, ingen strek

```c
static const uint8_t RIFT_MARK_16[] PROGMEM = {
  0x00,0x00, 0x1F,0xE0, 0x1F,0xE0, 0x18,0x18,
  0x18,0x18, 0x18,0x18, 0x18,0x18, 0x1F,0xE0,
  0x1F,0xE0, 0x19,0x80, 0x19,0x80, 0x18,0x60,
  0x18,0x60, 0x18,0x18, 0x18,0x18, 0x00,0x00
};
```

### 24×24 — skjøten overlever

Nedre grense. R i 3× (15×21), delt langs skjøten med 2px forskyvning; strek tegnes over
bitmapen etterpå.

```c
static const uint8_t RIFT_MARK_24[] PROGMEM = {
  0x07,0xFF,0x80, 0x07,0xFF,0x80, 0x07,0xFF,0x80, 0x07,0x00,0x70,
  0x07,0x00,0x70, 0x07,0x00,0x70, 0x07,0x00,0x70, 0x07,0x00,0x70,
  0x07,0x00,0x70, 0x07,0xFF,0x80, 0x07,0xFC,0x00, 0x07,0xC0,0x00,
  0x04,0x02,0x00, 0x00,0x3E,0x00, 0x00,0x70,0x00, 0x1C,0x70,0x00,
  0x1C,0x70,0x00, 0x1C,0x0E,0x00, 0x1C,0x0E,0x00, 0x1C,0x0E,0x00,
  0x1C,0x01,0xC0, 0x1C,0x01,0xC0, 0x1C,0x01,0xC0, 0x00,0x00,0x00
};
```

Streken, i markens egne koordinater: `drawLine(x+0, y+14, x+23, y+8)` i aksent, tegnet to
ganger med y+1 for 2px tykkelse. Radene 12..14 i bitmapen ser oppstykket ut alene — det er
skjøtekuttet, og streken legger seg i gapet.

Navbaren får ingen av dem: ved 12px blir R-en 5×7 og skjøten forsvinner i to piksler.
Der står `RIFT` som tekst, slik spesifikasjonen alt sier.

---

## COMMS — hoppetallet flyttes (fra 4c)

Én endring, uavhengig av resten.

`UITask::newMsg` dekorerer innkommende oppføringer med hoppantall foran navnet — `(6) #test:`
eller `(D) Bob:`. På avsenderlinjen står klokkeslettet først, så `14:52 (6) Public:` leses som
to klokkeslett.

Flytt hoppantallet til høyre ende av avsenderlinjen, der leveringsstatus alt står: begge svarer
på «hva skjedde med denne pakken».

- Klokkeslett x=2 i `dim`, bredde 36px (én tom celle etter).
- Navn fra x=38 i kanalens farge, eller `mid` når kanalen ikke har en.
- Høyrejustert x=318: `ACK 1.2s` i `ok`, `NO ACK` i aksent, ellers `n hops` i `mid`.
  På kanalmeldinger finnes ingen ACK-sannhet, så der står hoppantallet alene.
- `(D)` blir `direct` i samme slot.

`riftOriginName` normaliserer alt bort i dag; den skal fortsatt gjøre det for navnet, og
hoppantallet leses fra samme dekorasjon i stedet for å kastes.
