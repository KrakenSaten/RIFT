# RIFT — handover, designrunde august 2026

Gjelder de fem on-device-skjermene i RIFT (`KrakenSaten/RIFT`, branch `rift-tdeck`).
Utgangspunktet var `DESIGN-HANDOFF.md`, `handoff.md`, `channel-colours.md` og fem
telefonbilder av kjørende firmware.

Alt her er tegnet på 320×240 med 6×8-cellen og 12px radavstand, med chrome slik koden
tegner den i dag: navbar y 226..239, ingen tittelbar.

---

## Hva som er avgjort

| Sak | Avgjort |
|---|---|
| NODES | **1c** — faste bøtter med søyler, rullbar liste, ruten utvider den valgte raden |
| SYSTEM | **1d** — to sider, alle rader beholdt, diagnostikk gruppert |
| Ordmerke | **2c** — bokstavene delt langs skråstillingen, aksentstrek i skjøten, kant til kant |
| Aldersformat | Relativ i listen (`12m`), absolutt i detaljraden når klokka er satt |
| Marken | R alene på 16×16; skjøten fra 24×24 og opp. Navbaren beholder `RIFT` som tekst |
| RADAR | Ingen endring. Skjermen fungerer |
| RIFT (hjem) | Én endring: `NODES 256` → `256 STORED · 2 HEARD` |
| COMMS | Én endring: hoppetallet flyttes til høyre på avsenderlinjen |

## Hva som ble forkastet, og hvorfor

- **NODES 1a** (detaljbar nederst) — koster fire listerader for informasjon som bare gjelder
  én node. 1c gir samme opplysning der noden står.
- **NODES 1b** (ruten som eget ark) — flest noder på skjermen, men ruten blir en egen skjerm
  å navigere til og fra for det som er ett tastetrykk unna i 1c.
- **SYSTEM 1e** (to kolonner med rullefelt) — minste inngrep, men ingen av kolonnene får mer
  plass enn i dag, og høyre kolonne er den som vokser.
- **Ordmerke 2a** (overstryking) — leses som «kansellert», og streken ender i tom luft.
- **Ordmerke 2b** (utgang) — fungerer, men krysser bokstavstammer; skjøten unngår det.
- **16×16 med skjøt** — pikslet og lest ut: den 2px brede streken spiser R-ens nedre venstre
  ben. Forkastet fordi resultatet ser bestemt ut uten å være det.
- **Punkt mellom hurtigtastene i RADAR-fotlinjen** — `ENTER: watch 3 · W: wave · S: src` er
  198px, og med `nothing transmitted` blir det under én glyffbredde mellom strengene.

## Nye regler denne runden

1. **Alle skjermstrenger er CP437.** Ingen `…`, ingen `—`, ingen `−`. Tre ASCII-punkt og `-`.
   Æ, Ø og Å finnes ikke i den engelske delen av tabellen, så skjermtekstene står på engelsk.
2. **`·` finnes på 0xFA** og må skrives som det kodepunktet i C-strengen, ikke som UTF-8 —
   ellers tegnes to urelaterte blokker. Den er brukt i `256 STORED · 2 HEARD` og i
   detaljraden i NODES.
3. **Statusgrønn må skifte verdi mellom modusene.** `#39CB00` er 2,16:1 mot hvitt.
   Dagverdien er `#428610` (4,52:1).
4. **Skjøt-ordmerket trenger ingen bitmap-asset** på splash: to tekstkall og en
   blankingsløkke, oppskrift i spesifikasjonen.

## Filer

| Fil | Innhold |
|---|---|
| `design/rift-nodes-system-spec.md` | Implementasjonsspesifikasjon: koordinater, tilstander, bitmaps, CP437-regelen |
| `RIFT NODES + SYSTEM skisser.dc.html` | Designdokumentet, seks runder. Åpnes i nettleser |
| `design/screens/*.png` | Renderinger av de avgjorte skjermene, 320×240 i 2× |
| `github.md` | Repo-tilknytning og skjermkart |

`design/screens/` inneholder:

- `nodes-1c-night.png` / `nodes-1c-day.png` — den valgte NODES-skjermen
- `system-1d-night.png` — SYSTEM side 1 og 2, natt
- `system-1d-page1-day.png` / `system-1d-page2-day.png` — samme i dagmodus
- `splash-2c-night-day.png` — ordmerket på splash, begge moduser
- `mark-16-plain.png` / `mark-24-seam.png` — marken, 12× og 8×
- `comms-4c.png` — COMMS med hoppetallet flyttet

Renderingene er referanse. **Mål fra spesifikasjonen, ikke fra pikslene** — bildene er
skalert 2× og PNG-ene er ikke enhetens rammebuffer.

## Ikke gjort

- COMMS-endringen er spesifisert men ikke bygget inn i en full skjermspesifikasjon; den er
  én rad i `RiftCommsScreen`.
- RADAR-fossefallet er urørt fra forrige runde.
- 24×24-marken er lest ut som bitmap, men ikke prøvd på maskinvare.
- Kanalfargene fra `channel-colours.md` er brukt i COMMS-skissen slik de er, uten ny sjekk.

## Til den som implementerer

Fire ting som er lette å miste:

1. Bøtteintervallene i NODES er **faste**. En kolonne som betyr noe annet fra gang til gang
   betyr ingenting.
2. Summen av de fire bøttene kan være mindre enn `n HEARD`. Noder uten kjent rute hører ikke
   i noen bøtte, og `?` i listen er stedet de finnes. Ikke fyll ut differansen.
3. Et tvetydig hoppbyte skal tegnes `?`. Aldri første kandidat.
4. Når den valgte raden utvider seg nær nedre kant, skal listen rulle så både raden og de to
   detaljradene er innenfor y ≤ 225. Et valg som ikke er tegnet finnes ikke for brukeren.
