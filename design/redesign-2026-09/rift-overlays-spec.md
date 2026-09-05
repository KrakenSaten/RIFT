# Overlegg — spesifikasjon

Felles: 00-SYSTEM §7 (fyll bg, ramme 2 px rule, innhold fra x+4 / y+4, hint ved y+h−14). Skjermen under tegnes uendret først. Renderinger: `discover-*`, `discover-empty-*`, `repeater-*`, `preview-*`, `namedevice-*`, `nordic-*`.

## 1. Oppdagelse (DISCOVER 0-HOP) — boks [6, 22, 308, 186], over RIFT

| Element | Koordinat | Rolle | Streng |
|---|---|---|---|
| Overskrift | text (10, 26) | mid | `DISCOVER 0-HOP · 16 REPEATERS`; underveis `DISCOVER 0-HOP · LISTENING` |
| Teller | textR (310, 26) | mid | `8s` nedtelling, så `done` |
| Kolonner | text (10, 38) `REPEATER`, text (220, 38) `RX SNR`, text (268, 38) `TX SNR` | mid | |
| Rader | 11 fra y 50 (siste topp 170) | fg | navn text (10,y); rx textR (256,y) `11.5`; tx textR (304,y) `9.0`, `?` når repeateren ikke rapporterte |
| Valgt | fillRect [8, y−2, 301, 12] | accent / on_accent | |
| Vindu | spor [309, 50, 1, 132], tommel x 308 w 2 | rule / fg | |
| Hint | text (10, 194) | mid | `ENTER: control · DBL-CLICK: close` |

Tom (ingen svar): rad y 50 `No repeater answered within 8 s.` fg; y 62 `0-hop asks direct neighbours only.` mid; y 74 `Try ADVERT NEAR, or move.` mid; hint `ENTER: retry · DBL-CLICK: close`. Ukjent tx-SNR er `?`, ikke tom. Full: 16 i listen, 11 synlige, tommel; ingen `+N more`.

## 2. Repeater-kontroll — boks [6, 10, 308, 206], over NODES

| Element | Koordinat | Rolle | Streng |
|---|---|---|---|
| Overskrift | text (10, 14) | mid | `SE-KSD-KAU1 Repeater · CONTROL`; modus: `· PASSWORD` `· COMMAND` |
| Auth-teller | textR (310, 14) | mid | `auth 27s` (hemmeligheter skjules etter 30 s), `no auth` |
| Statistikk | 5 rader fra y 26: etikett text (10,y) mid, verdi textR (310,y) fg | | `UPTIME` `4d 03h 12m` · `AIRTIME` `2.1% · DUTY 0.8%` · `BATT` `4.02 V` · `RX / TX / DUP` `1203 / 418 / 22` · `TEMP / HUM` `18.5 °C · 62 %` (Cayenne LPP med enheter; ukjent felt = `?`) |
| Menyoverskrift | text (10, 90) | mid | `COMMAND` |
| Meny | 5 rader fra y 102; valgt [8, y−2, 301, 12] accent | fg / on_accent | `Status` `Set clock from mine` `Advert` `Reboot` `Set password` |
| Menyhint | textR (310, y) på valgt rad | on_accent | destruktiv: `ENTER: arm` → armert: `ENTER AGAIN TO FIRE`; ikke-destruktiv: `ENTER: send` |
| Skille | fillRect [8, 162, 301, 1] | rule | |
| CLI | 5 linjer fra y 166, pitch **10** (rullebuffer, ikke layout) | mid; siste svar fg | `> status` … |

Armert tilstand holdes som radindeks (handover 5.4); flyttes markøren, faller hintet tilbake til `ENTER: arm`. Passordmodus: rad y 102 `>` og `********`, hint `ENTER: send · shown 30 s after auth`. Feil: CLI-linje i accent/accent_txt `! auth failed`.

## 3. Meldingsforhåndsvisning — boks [6, 40, 308, 110], over enhver skjerm

| Element | Koordinat | Rolle | Streng |
|---|---|---|---|
| Overskrift | text (10, 44) mid `6 NEW MESSAGES`; textR (310, 44) mid kanalen med flest | | |
| Rader | 6 fra y 56: tid text (10,y) dim; navn text (46,y) navnefarge; tekst text (106,y) fg, 34 tegn, kuttet uten `...` | | |
| Hint | text (10, 134) mid | | `ENTER: open COMMS · DBL-CLICK: close` |

Tom finnes ikke (overlegget reises bare med ≥1 ulest). Én ulest: `1 NEW MESSAGE`, én rad.

## 4. Navngi enhet — boks [6, 70, 308, 86], over RADAR

| Element | Koordinat | Rolle | Streng |
|---|---|---|---|
| Overskrift | text (10, 74) | mid | `NAME DEVICE · 5C:F3:70:A1:22:0C` |
| Felt | text (10, 92) `>` fg; tekst fra x 22 fg med `_` markør | | |
| Teller | textR (310, 92) | mid | `9/24` |
| Forklaring | text (10, 110), (10, 122) | mid | `Named devices are watched: a lamp on` / `RADAR when they appear.` |
| Hint | text (10, 140) | mid | `ENTER: save · DBL-CLICK: cancel` |

Full (12 overvåkede): forklaringen byttes med `Watch list full (12). Forget one first.` i fg; ENTER lagrer navn uten å overvåke. Tomt felt + ENTER: fjerner navn og overvåking.

## 5. Nordisk velger — boks [10, 184, 68, 20], over COMMS-skrivelinjen

Reises ved dobbelttrykk på vokal. Fire celler 12×12 ved x `14 + 15i`, y 188: drawRect rule + glyf fg; valgt: fill accent + on_accent. Innhold per vokal: `a å ä æ` · `o ø ö` (3 celler, boks w 53) · `u ü` (2, w 38) · `e é è` (3). LEFT/RIGHT flytter, ENTER setter inn, alt annet lukker uten innsetting. Boksen ligger over historikkens nederste rad og tegnes med ett fyll + to rammer; ø/Ø krever glyfene i 00-SYSTEM §9.

## 6. Oppstart (ikke overlegg)

Uendret: ordmerke text (32, 78) størrelse 3, skjøt fra x 0 til 160 langs 12/78; `R A D I O   I N T E L L I G E N C E` (32, 110) mid; `&   F I E L D   T E R M I N A L` (32, 122) mid. Statuslinje text (32, 150) mid bare når SPIFFS formateres: `formatting storage`.
