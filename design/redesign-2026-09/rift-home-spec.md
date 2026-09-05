# RIFT (hjem) — spesifikasjon

Spørsmål: *Er meshet levende der jeg står?* Rendering: `renders/home-{night,day}-2x.png`, tom: `home-empty-*`.
Felles regler: `00-SYSTEM.md`.

## Layout

| # | Element | Koordinat | Rolle natt / dag | Streng |
|---|---|---|---|---|
| 1 | Kilde | text (2, 2) | mid | `meshcore.io` |
| 2 | Statusord | text (2, 20) størrelse 3, høyde 24 | ACTIVE: ok · IDLE: fg · QUIET: dim · NO SIGNAL: accent / accent_txt | `ACTIVE` `IDLE` `QUIET` `NO SIGNAL` |
| 3 | Ordmerke | text (244, 20) størrelse 3, `RIFT`; skjøt 4 px bg med 2 px accent langs 12/78 fra x 176 til 316, y 26→37 | fg + accent | uendret |
| 4 | Siste mottak | text (2, 58) | fg | `LAST RX 58s AGO` |
| 5 | Pakketall | text (2, 70) | fg | `RX 11 PACKETS` |
| 6 | Lenke | text (2, 82) | mid | `USB/BLE CONNECTED` · `BLE CONNECTED` · `NO HOST` |
| 7 | Stripeetikett | text (2, 100) | mid | `HEARD, LAST 20 MIN` |
| 8 | Link | textR (316, 100) | mid | `RSSI -80  SNR -4` |
| 9 | Aktivitetsstripe | 20 celler 8×8 ved x `2 + 12i`, y 112, i = 0..19 (eldste venstre) | fg | full = ≥1 pakke hørt det minuttet, hollow = ingen |
| 10 | Lager | text (2, 136) | mid | `258/350 STORED · 3 HEARD` |
| 11 | Radio | text (2, 148) | mid | `869.618MHz SF8 22dBm` |
| 12 | Tropo | text (2, 160) | lukket: mid · åpen: fg | `TROPO none since boot` · `TROPO closed 2h ago` · `TROPO OPEN · peak 6 hops` |
| 13 | Knapperad | y 180, h 14: `DISCOVER 0-HOP` [2,180,92,14], `ADVERT NEAR` [100,180,74,14], `ADVERT MESH` [180,180,74,14] | valgt accent/on_accent, andre rule/fg | |
| 14 | Note | text (2, 200) | mid | `asks direct neighbours only` · `flood advert, 1 hop` · `flood advert, full mesh` |
| 15 | Nav | RIFT aktiv | | |

Ingenting tegnes ved y 212–225. Bare 9 (og 4/5/8 når verdiene endres) tegnes på timer; stripen oppdateres én gang i minuttet og koster 20 fyll.

## Tilstander

| Tilstand | Endring |
|---|---|
| Tom (ingen pakke siden boot) | 2 = `NO SIGNAL`; 4 = `LAST RX none since boot`; 5 = `RX 0 PACKETS`; 8 = `RSSI --  SNR --`; 9 alle hollow; 10 = `0/350 STORED · 0 HEARD` |
| QUIET (>10 min siden RX) | 2 = `QUIET` i dim; stripen viser det samme i form (høyre celler hule), så ordets gråtone er ikke eneste bærer |
| Tropo åpen | 12 i fg med `TROPO OPEN · peak N hops`; ingen annen rad erstattes |
| Ukjent link (ingen RX ennå) | 8 = `RSSI --  SNR --` (to bindestreker, ikke 0) |
| Discover kjører | knapp 1 forblir valgt; overlegget (se overlays-spec) tegnes over; skjermen under urørt |
| Batteri ≤ 15 % | bare nav-linjen endres |

## Endret fra dagens skjerm

- Radaranimasjonen (konsentriske kvadrater + bane-prikk, tegnet på timer) er fjernet. Erstattet av aktivitetsstripen (9): reelle data, 20 fyll per minutt i stedet for ~20 fyll per bilde, og den svarer bokstavelig på skjermens spørsmål.
- TROPO har egen rad (12) og erstatter ikke radioparameterlinjen. En rad som bytter mening er en modus brukeren må oppdage.
- Knapperaden flyttet fra y 198 til y 180, noten fra y 214 til y 200. Nederste 14 px før nav-linjen er tomme; nederste tredjedel har igjen luft.
- `LINK -80 / -4` er skrevet ut som `RSSI -80  SNR -4`; to tall uten navn var det ene stedet på skjermen som krevde forkunnskap.
- Statusordets farger uendret, men QUIET bæres også av stripen (regel 2).
