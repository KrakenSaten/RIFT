# NODES — spesifikasjon

Spørsmål: *Hvem er der ute, og hvor langt unna?* Rendering: `renders/nodes-{night,day}-2x.png`, tom: `nodes-empty-*`.

## Layout

| # | Element | Koordinat | Rolle | Streng |
|---|---|---|---|---|
| 1 | Overskrift venstre | text (2, 2) | mid | `3 RECENT · 14 NODES` |
| 2 | Overskrift høyre | textR (316, 2) | mid | `MAX 10 HOPS` |
| 3 | Bøtteetiketter | text (2+63i, 14), i = 0..4 | mid | `DIRECT` `1-2` `3-5` `6+` `NO ROUTE` |
| 4 | Bøttespor | drawRect [2+63i, 24, 58, 4] | rule | alltid tegnet (hul = null) |
| 5 | Bøttesøyle | fillRect [2+63i, 24, max(2, round(n/max·58)), 4] | fg | sammenlignes innbyrdes; max = største bøtte |
| 6 | Bøttetall | text (2+63i, 30) | fg | `5` |
| 7 | Kolonneoverskrifter | text (2,44) `NODE`, text (140,44) `REACH`, textR (284,44) `HOPS`, textR (314,44) `AGE` | mid | |
| 8 | Liste | rader fra y 56, pitch 12, 14 rader (siste topp y 212) | | |
| 8a | Ferskhet | [2, y+1, 5, 5] fill = hørt siste 30 min, drawRect = eldre | fg / on_accent | |
| 8b | Navn | text (10, y), maks 20 tegn (x 10–130), avkortet `17 tegn + ...` | fg / on_accent | translitert, se 00-SYSTEM §9 |
| 8c | Rekkevidde | 10 celler 8×8 ved x `140 + 12i`, y | fg / on_accent | celle i < hops: **full** hvis ruten er bekreftet, **dot** hvis ubekreftet; ellers hollow. Ukjent rute: 10 hollow |
| 8d | Hopp | textR (284, y) | fg / on_accent | eksakt tall, `?` ved ukjent, `12` ved >10 (alle ti celler fylt) |
| 8e | Alder | textR (314, y) | fg / on_accent | `3m` `2h` `4d` `>99d` `?` (klokke ikke satt) |
| 9 | Valgt rad | fillRect [0, y−2, 316, 36] (rad + 2 utvidelsesrader) | accent | blekk on_accent |
| 9a | Rute | text (10, y+12) | on_accent | `via RP2 » Tyri-Asen Solar » RP7`; ukjent: `route unknown · heard by flood` |
| 9b | Handling | text (10, y+24) og textR (314, y+24) | on_accent | `ENTER: message` / `ENTER: control` / `ENTER: read` · `RIGHT: control` når noden er repeater |
| 10 | Vindu | spor [319,56,1,170], tommel x 318 w 2 | rule / fg | 00-SYSTEM §4 |
| 11 | Nav | NODES aktiv | | |

## «Bekreftet» og «ubekreftet» (8.1)

- Bekreftet: ruten er brukt i en levering (ACK mottatt) eller er path-hash for en pakke vi faktisk mottok fra noden de siste 30 minuttene.
- Ubekreftet: hoppantallet stammer bare fra en advert eller en eldre path-hash. Tallet står, men cellene sier at vi ikke har sett veien virke.
- Ukjent: ingen path. Ti hule celler og `?`. Aldri 0.

## Tilstander

| Tilstand | Endring |
|---|---|
| Tom | 1 = `0 NODES`; 4 tegnes hule, 6 = `0`; rad 1 (y 56) `NO NODES HEARD SINCE BOOT` i mid, rad 2 `ADVERT NEAR on RIFT asks neighbours` i mid; ingen tommel |
| Ukjent rute | 8c ti hollow, 8d `?`, bøtte NO ROUTE teller den |
| Valgt | 9, 9a, 9b; radene under flyttes 24 px ned; markøren trekker vinduet så alle tre radene er synlige |
| Full (>14 noder) | tommel vises; utvidet rad regnes som 3 rader i `view` |
| Klokke ikke satt | 8e `?` for alle; overskrift høyre blir `CLOCK NOT SET` |
| >10 hopp | ti fulle/dot-celler, eksakt tall i 8d; MAX-verdien i 2 oppdateres |
| Feil (path-hash tvetydig) | 9a `route ambiguous · 2 candidates`; celler dot |

## Endret fra dagens skjerm

- Kolonneoverskrifter flyttet fra y 56 til y 44, liste fra y 68 til y 56: én rad mer (14), samme geometri som SYSTEM/RADAR-lister (topp 56 = 44 + 12).
- `HEARD` → `AGE`, høyrestilt; `HOPS` og `HEARD` kolliderte ved x 266/290 og HEARD gikk til x 320.
- Cellene får tre former (full / dot / hollow) — svaret på 8.1, se ANSWERS.
- Bøttesporet tegnes alltid (hult), så en tom bøtte er en form og ikke fravær.
- Tommel ved x 318–319; radene slutter ved x 316.
- Navn translitereres (00-SYSTEM §9).
- `RIGHT: control` skrives ut i handlingsraden når noden er en repeater; tidligere var det bare kjent for den som hadde lest dokumentasjonen.
