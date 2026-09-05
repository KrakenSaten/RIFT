# RIFT redesign 2026-09 — felles system

Gjelder alle skjermer og overlegg. Skjermspesifikasjonene refererer hit i stedet for å gjenta.
Alle koordinater er enhetspiksler, 320×240, origo øverst til venstre. `[x, y, w, h]`.
Palettroller er fra handover del 3; **ingen nye roller er lagt til**. Ett unntak: `ok` i dagmodus får ny verdi (se ANSWERS-8.md §8.3).

## 1. Rutenett

| Element | Regel |
|---|---|
| Radavstand | 12 px. Radtopp `y = 2 + 12k` for overskrift/liste uten fane, `y = 16 + 12k` for lister under en 14 px-topp |
| Glyf | 6×8, tekst tegnes ved radtopp; rad-fyll (valgt rad) dekker `[0, y-2, 316, 12]` |
| Venstremarg | Tekst x 2. Rad med markørtegn: markør x 2, tekst x 10 |
| Høyremarg | Tekst slutter ved x 314 (høyrestilt). x 316–319 er reservert vindusmønsteret |
| Overskrift | Én linje ved y 2 i `mid`, bare når layouten ikke sier det selv. Venstre: tilstand/antall. Høyre: grense eller hint |
| Fotlinje | Én linje ved y 214 i `mid` for tastehint, bare på skjermer med mer enn én handling. Lister som bruker fotlinjen slutter ved y 208 |
| Kropp | y 0–225. Navigasjonslinje y 226–239, uendret |

## 2. Navigasjonslinje (uendret geometri, én endring)

- `[0,226,320,1]` rule; `[0,227,320,13]` bar
- Etiketter y 228 sentrert x 20, 81, 145, 209, 270 i `mid`
- Aktiv: `accent` tekst (natt) / `fg` tekst (dag) + `[x0,237,w+8,2]` accent der w er etikettbredden. RIFT aktiv i dag: aksentbrikke `[x0,227,w+8,13]` med `on_accent` tekst
- Batteri høyrestilt til x 318, y 228, `mid`; ≤15 %: `accent` (natt) / aksentbrikke med `on_accent` (dag)
- **Endret:** ulest-merke `[240,231,3,3]` er `fg`, ikke accent (ANSWERS §8.4)
- **Endret:** SYSTEM viser batteri som alle andre; sidetallet er borte fordi SYSTEM ikke lenger har sider

## 3. Former (regel 2: form overlever sollys)

| Form | Tegning | Betyr |
|---|---|---|
| `full` 8×8 | fillRect 8×8 | telt / bekreftet / aktiv i dette minuttet |
| `hollow` 8×8 | drawRect 8×8 | slot uten verdi |
| `dot` 8×8 | drawRect 8×8 + fillRect `[+2,+2,4,4]` | verdi finnes men er ubekreftet |
| `full6` / `hollow6` 6×8 | fill/drawRect 6×8 | RADAR: én enhet, nær/middels vs fjern |
| Markør 5×5 | fill/drawRect `[2,y+1,5,5]` | fersk / eldre (NODES), overvåket (RADAR) |
| Ulest 3×3 | fillRect 3×3 `fg` | ulest. Fravær = lest |
| Brikke 2×12 | fillRect `[7,y-2,2,12]` | kanal- eller avsenderidentitet i lister |
| Strek 2×h | fillRect `[0,y-2,2,h]` `fg` | egen melding (COMMS) |
| Overflyt | glyf 0xAF `»` i `fg` | flere enn det er plass til, i rekker uten markør |
| Ukjent | glyf `?` | ukjent verdi, aldri erstattet med 0 eller tom |

## 4. Vindusmønster for alle lister med markør (svar på 8.6)

Brukes av NODES, RADAR (enhetsliste og overvåkningsliste), COMMS-historikk, samtalelisten, SYSTEM, oppdagelsespanelet, repeater-menyen og hendelseslogg/air log.

1. **Viewport** er et helt antall rader (`view`). Ingen rad tegnes delvis; det som ikke får plass tegnes ikke (COMMS er unntaket, se 5.1: pikselrulling med klipping ved vannrette kanter).
2. **Markøren trekker vinduet.** Flyttes markøren under siste synlige rad, flyttes `first` ett steg; over første, ett steg tilbake. Drag flytter `first` direkte (16 px per rad), og markøren holdes innenfor vinduet. Ingen rundbryting under drag.
3. **Tommel**, tegnet kun når `total > view`:
   - spor `[319, y0, 1, len]` i `rule`, der `y0`/`len` er listens synlige topp og høyde
   - tommel `[318, y0 + round(first/total·len), 2, max(6, round(view/total·len))]` i `fg`
   - inne i et overlegg ligger sporet ved `x = boks.x + boks.w − 5` og tommelen ved `x − 1`, w 2
4. **Ingen «+N more»-tekst.** Antallet står i overskriftslinjen (`16 REPEATERS`, `12 conversations`, `14 NODES`). Tommelen sier hvor du er.
5. **Seksjonsoverskrifter** tar én rad, kan ikke velges, og hoppes over av markøren. De teller i `total`.
6. **Berøringsmål** er raden slik den ble tegnet (5.2 i handover). Tommelen er ikke berøringsmål; sporet er 1 px og kan ikke treffes med finger, så rullingen er alltid drag på radene.
7. **Tom liste** tegner ingen tommel og én til to `mid`-linjer ved første radposisjon som sier hvorfor den er tom (regel 7).

## 5. Rullemodeller (8.9)

5.1 COMMS-historikk ruller pikselvis, klippes ved y 16 og y 204 (to fyll i full bredde + chrome-omtegning). Den bruker samme tommel (`y0 16`, `len 188`) med `first/total/view` i piksler.
5.2 Alle andre lister stepper radvis. Se ANSWERS §8.9 for hvorfor begge beholdes.

## 6. Aksentens betydninger etter denne runden (8.4)

Beholdt (fire): aktiv fane · valgt/handlingsbar rad, knapp eller celle · varsel (NO SIGNAL, batteri ≤15 %, overvåket enhet, `no ack`, tap-advarselens ramme, armert kommando) · ordmerkets 2 px.
Flyttet til form i `fg`: egen melding (2 px strek), ulest (3×3 kvadrat).
Flyttet til `rule`: overleggsramme (2 px, to nestede drawRect).

## 7. Overlegg

- Fyll `bg`, ramme `rule` 2 px: `drawRect(x,y,w,h)` + `drawRect(x+1,y+1,w−2,h−2)`
- Innhold starter ved `x+4`, første rad `y+4`; siste linje er et tastehint i `mid` ved `y+h−14`
- Skjermen under tegnes først, uendret, og leveres tilbake urørt
- Valgt rad i et overlegg fyller `[x+2, y−2, w−4, 12]`

## 8. Knapp

`[x, y, len·6+8, 14]`. Valgt: fill `accent`, tekst `on_accent` ved `(x+4, y+3)`. Uvalgt: drawRect `rule`, tekst `fg`. Avstand 6 px. Alle knapper har en tastevei (piltaster + ENTER).

## 9. Tekst

- CP437, engelsk. `·` = 0xFA, `√` = 0xFB, `»` = 0xAF, `«` = 0xAE.
- Ingen `…`; avkorting er tre punktum `...` innenfor kolonnens bredde (navn 20 tegn: 17 + `...`).
- **Innkommende UTF-8** (nodenavn, meldinger) transliteres før tegning: å→0x86, ä→0x84, æ→0x91, ö→0x94, ü→0x81, Å→0x8F, Ä→0x8E, Æ→0x92, Ö→0x99, Ü→0x9A, é→0x82, è→0x8A. **ø og Ø finnes ikke i CP437**; foreslått: to håndtegnede 5×7-glyfer i slot 0xED (φ, ubrukt) og 0xE8 (Φ, ubrukt), kolonnedata `78 64 54 4C 3C` og `7E 61 51 49 3F`. Alt annet ukjent → `?`. Skjermbildet nodes-night fra 5. sept viser hva som skjer uten dette (`Tyri¦Åsen`).

## 10. Palettroller per flate (felles)

| Flate | Natt | Dag |
|---|---|---|
| Felt | bg | bg |
| Nav-bånd / hårstrek | bar / rule | bar / rule |
| Primærtekst, tall, navn | fg | fg |
| Etiketter, overskrifter, hint, sekundærverdi | mid | mid |
| Klokkeslett i COMMS | dim | dim |
| Rammer, spor, uvalgt knapp, hul celle-kant i overlegg | rule | rule |
| Valgt rad/knapp/celle | accent + on_accent | accent + on_accent |
| Varseltekst | accent | accent_txt på bg; on_accent på aksentbrikke |
| Levert, ACTIVE | ok #39C800 | ok **#2A6400** (ny, se ANSWERS §8.3) |
| Kanalfarger 1–4, navnefarger | uendret, samme i begge | |
