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
| 9 | Aktivitetsstripe | 20 søyler 8 px brede ved x `2 + 12i`, bunn y 126, høyde 1–16 px etter `riftActivityHeight` (eldste venstre) | TALK ok · ADV `airTypeColour(0x04)` · OTHER mid | stablet nedenfra i den rekkefølgen; nullminutt tegner bare grunnlinjesegmentet |
| 9b | Fargenøkkel | textR (316, 136) | i klassens egen farge | `TALK ADV OTHER`, bare når stripen har søyler |
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

## Aktivitetsstripen etter spec-en

Rad 9 over beskriver hva som faktisk kjører. Stripen har endret seg tre ganger siden
denne spec-en ble skrevet, og de to siste gangene fordi den var målbart feil.

**Fra hule celler til høyder og tre klasser.** Spec-en ba om én bit per minutt — hørt
noe, eller ikke. Det svarer på skjermens spørsmål, men ikke på «hvor mye», og et
minutt med én advert så ut som et minutt med femti. Høyde og tre klasser kom til.

**Bort fra topprelativ skalering.** Høyden var først minuttets andel av det travleste
minuttet i vinduet. Aritmetikken var riktig og resultatet var likevel uleselig: ved en
topp på 2/min sto et ettpakkes minutt 8 px høyt, og i det et trepakkes minutt kom inn
tegnet *det samme minuttet* seg 6 px. Hele grafen krympet fordi nevneren flyttet seg,
og den vokste igjen når et travelt minutt rant ut av vinduet. Fortiden beveget seg.
Dette er samme feil NODES-notatet advarer mot i §6: en kolonne hvis betydning følger
nettet er en kolonne du ikke kan lese.

Erstattet av en fast stige med doblende trinn — 1 / 2-3 / 4-7 / 8-15 / 16+ — fordi
trafikk fordeler seg slik: et mesh går på tomgang med én eller to pakker i minuttet og
bykser til dusinvis. Ingenting klipper; det eksakte tallet for travleste minutt står i
etiketten. En søyle endrer seg nå bare når dens eget minutt gjør det.

**Bort fra å utlede stripen av luftloggen.** Den ble regnet ut ved å gå gjennom
`RiftRxLog` hver ramme og bøtte funnene etter alder. Ringen tar 64 oppføringer for
begge retninger, altså tjue minutter bare så lenge trafikken holder seg under 3,2
pakker i minuttet — målt akkurat full, 62 rx + 2 tx, på et mesh som gjorde omtrent to.
Over det mistet de eldste søylene bevismaterialet sitt til utkastelse og krympet, noe
som ser ut som at meshet var stille den gangen. Stripen har nå egne tellere:
`RiftActivity` i `RiftLogic.h`, 120 byte, matet fra `RiftRxLog::add()` og rullet både
på pakkestien og ved tegning. Ingenting kan kaste dem ut.

**Nøkkelen (9b)** kom fordi fargene ikke hadde noen. TALK og ADV tegnes i de samme
verdiene `airTypeColour()` gir 0x02 og 0x04, så en farge her og samme farge i
luftloggen betyr samme trafikk. OTHER er mid, som også er vanlig etikettfarge — den
klassen *er* resten, og en egen farge ville hevdet at den var en kategori noen valgte.
