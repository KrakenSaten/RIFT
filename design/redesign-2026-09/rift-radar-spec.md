# RADAR — spesifikasjon

Spørsmål: *Hvilke Wi-Fi- og BLE-enheter er rundt meg?* Rendering: `renders/radar-{night,day}-2x.png`. Overlegg «navngi enhet»: `namedevice-*`.

## Layout

| # | Element | Koordinat | Rolle | Streng |
|---|---|---|---|---|
| 1 | Overskrift | text (2, 2) | mid | `WIFI+BLE · 5s SCAN` · `WIFI · 5s SCAN` · `BLE · 5s SCAN` |
| 2 | Overvåket-lampe | fillRect [316−w, 0, w, 12], w = len·6+8; text (316−w+4, 2) | accent / on_accent (begge moduser) | `WATCHED: Garmin-4F`; flere: `WATCHED: 2` |
| 3 | Antall | text (2, 16) størrelse 3 | fg | `23` |
| 4 | Enhet | text (2+18·len+6, 32) | mid | `DEVICES` · `DEVICE` |
| 5 | Bånd | rader y 48, 60, 72: etikett text (2, y) fg; område text (38, y) dim; celler 6×8 ved x `92 + 8k`, k = 0..27 | fg | `CLOSE` `> -60` · `MID` `-60/-75` · `FAR` `< -75`. CLOSE/MID full6, FAR hollow6 |
| 5a | Overflyt | glyf `»` (0xAF) ved x 92+8·27, y | fg | når n > 28; erstatter 28. celle |
| 6 | Kolonneoverskrifter | text (2,88) `DEVICE`, text (254,88) `RSSI`, text (290,88) `SEEN` | mid | |
| 7 | Liste | 9 rader fra y 100 (siste topp 196), sortert sterkest først | | |
| 7a | Kilde | text (2, y) | mid / on_accent | `W` `B` |
| 7b | Navn | text (14, y), maks 37 tegn | fg / on_accent | navngitt: navnet; ellers `unnamed 5C:F3:70` |
| 7c | Overvåket | fillRect [243, y+1, 5, 5] | fg / on_accent | bare på overvåkede |
| 7d | RSSI | textR (272, y) | fg / on_accent | `-48` |
| 7e | Sett | textR (314, y) | fg / on_accent | `now` `2s` `40s` |
| 8 | Valgt rad | fillRect [0, y−2, 316, 12] | accent | |
| 9 | Vindu | spor [319, 100, 1, 126], tommel x 318 | rule / fg | |
| 10 | Fotlinje | text (2, 214) | mid | `ENTER: waterfall  N: name  S: source` |
| 11 | Nav | RADAR aktiv | | |

## Overvåkningsliste (holder 12)

Åpnes fra fotlinjen: `N` på en rad navngir (og overvåker) den; `LEFT` åpner listen. Tegnes som overlegg [6, 22, 308, 186] etter 00-SYSTEM §7:
- overskrift text (10, 26) mid `WATCHED · 12 DEVICES`; kolonner y 38 `NAME`, textR (256) `LAST SEEN`, textR (304) `RSSI`
- 11 rader fra y 50, valgt rad [8, y−2, 301, 12] accent
- tommel spor x 309, tommel x 308 w 2 (00-SYSTEM §4.3)
- fotlinje text (10, 194) mid `ENTER: rename · DEL: forget · DBL-CLICK: close`
- tom: `No watched devices. N on a RADAR row names one.`
Rader 7–12 tegnes altså aldri over fotlinjen; de ligger bak tommelen og trekkes inn av markøren.

## Fossefall (ENTER)

Uendret i innhold. Bruker kroppen y 16–208 og samme fotlinje ved y 214: `ENTER: list  W: waterfall  S: source`.

## Tilstander

| Tilstand | Endring |
|---|---|
| Tom (skann uten funn) | 3 = `0`, 4 = `DEVICES`; båndene tegner ingen celler; rad 1 `No devices in the last scan.` mid, rad 2 `S switches source; walls cost 20 dB.` mid |
| Skann pågår, ingen første resultat | 3 = `--`; 1 = `WIFI+BLE · SCANNING` |
| Full (>9 enheter) | tommel; overskriften teller alle |
| Overflyt i bånd | 5a |
| Overvåket enhet til stede | 2 tegnes; raden får 7c |
| Feil (radio opptatt) | 1 = `WIFI OFF · LORA BUSY` i mid; ingen skann |

## Endret fra dagens skjerm

- Overflytcellen i aksent er byttet med `»`-glyfen i fg. Overflyt er ikke et varsel, og aksenten er opptatt (8.4).
- dBm-grensene står i dim ved siden av båndnavnet i én kolonne (x 38), så cellerekkene begynner på samme x i alle tre bånd.
- Enhetslisten er 9 rader og har tommel; fotlinjen ved y 214 samler de tre tastene som før var usynlige.
- Overvåket er en 5×5 fylt form i egen kolonne, ikke bare farge på navnet.
- Overvåkningslisten er lagt inn i vindusmønsteret (svar på 8.6): 12 oppføringer, 11 synlige, ingen tegnes over nav-linjen.
