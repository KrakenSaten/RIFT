# Endringsliste — redesignrunde september 2026

Hver endring: hva, fra dagens skjerm (del 6 i handover, skjermbilder 5. sept), og hvorfor. Regelnummer viser til del 7.

## Felles

| Endring | Fra | Hvorfor |
|---|---|---|
| Ett vindusmønster (00-SYSTEM §4) | tre ulike: vindu i samtalelisten, overtegning i RADAR-overvåkning, avkortet liste i oppdagelse | 8.6; regel 5 og 10 |
| Tommel x 318–319, tekst slutter 314 | tekst til 316–320 | plass til tommelen uten å ta et tegn fra noen kolonne |
| Ulest-merke 3×3 i fg | accent | 8.4; regel 8 |
| Egen melding 2 px strek i fg | accent | 8.4 |
| Overleggsramme 2 px rule | 1 px accent | 8.4 |
| Tastehint i mid overalt | accent på NODES (`ENTER: message`) | aksent i tekst betyr nå bare varsel |
| Tom tilstand med grunn på alle lister | varierte | regel 7 |
| `?` for ukjent overalt, `--` for «ikke målt ennå» | 0 eller tomt enkelte steder | regel 4 |
| Transliteringstabell + ø/Ø-glyfer | rå UTF-8 | regel 11; funn i nodes-night |
| `ok` dag = #2A6400 (0x2B20) | #428610 | 8.3, tall i ANSWERS |
| Overskrift y 2 bare når nødvendig, fotlinje y 214 bare ved >1 handling | ulik praksis | felles rytme |

## RIFT (hjem)

| Endring | Fra | Hvorfor |
|---|---|---|
| Aktivitetsstripe 20 celler, y 112 | radaranimasjon på timer | svarer på skjermens spørsmål med data; billigere på SPI-bussen |
| TROPO egen rad y 160 | erstattet radioparameterlinjen | en rad med to meninger er en modus |
| Knapperad y 180, note y 200 | y 198 / y 214 | luft mot nav-linjen; 8.2 |
| `RSSI -80  SNR -4` | `LINK -80 / -4` | eneste tall uten navn |
| `LAST RX none since boot` | `LAST RX --` | regel 7 |

## NODES

| Endring | Fra | Hvorfor |
|---|---|---|
| Celleform full/dot/hollow = bekreftet/ubekreftet/ingen | full/hollow | 8.1 |
| Kolonneoverskrift y 44, liste y 56, 14 rader | y 56 / y 68, 13 rader | felles rytme med RADAR/SYSTEM; én rad mer |
| `AGE` høyrestilt 314; `HOPS` høyrestilt 284 | `HEARD` x 290 (til 320), `HOPS` x 266 | overlapp og kant |
| Bøttespor tegnes alltid (hul) | ingen spor ved null | tom bøtte er en form |
| `RIGHT: control` i handlingsraden | udokumentert | regel 9 |
| Ukjent rute: `route unknown · heard by flood` i utvidelsen | tom | regel 4/7 |

## RADAR

| Endring | Fra | Hvorfor |
|---|---|---|
| Overflyt = `»` i fg | aksentcelle | overflyt er ikke varsel |
| dBm-grense i egen kolonne x 38, celler fra x 92 i alle bånd | grensene i etiketten | rekkene starter likt |
| Enhetsliste 9 rader + fotlinje `ENTER: waterfall  N: name  S: source` | usynlige taster | regel 9 |
| Overvåket = 5×5 fylt i kolonne x 243 | farge på navn | regel 2 |
| Overvåkningsliste i overlegg med vindu (12/11) | 6 vist, 7–12 over nav-linjen | 8.6; regel 5 |

## COMMS

| Endring | Fra | Hvorfor |
|---|---|---|
| `√ N hops` i ok for levert | ord i grønt | 8.3 |
| `no ack` i accent etter 60 s | — | eneste varsel i historikken |
| Fane 0 og ≥5 rammet i rule; ulest 3×3 i fane | — | 8.8, 8.4 |
| Stripe-overflyt `«` `»` | ingen | 8.8 |
| Tegntelling `24/160` i høyre slot under skriving | bare hint | fylt tilstand synlig |
| Tommel i historikk og samtaleliste | bare vindu i samtalelisten | 8.6 |
| Kanalbrikke i rule for kanal ≥5 i samtalelisten | — | 8.8 |

## SYSTEM

| Endring | Fra | Hvorfor |
|---|---|---|
| Én liste, fem grupper, 33 rader / 17 synlige | side 1 handlinger + lesninger, side 2 grupper | én rullemodell på enheten; 8.6 |
| Verdi høyrestilt 314 på alle rader | varierte | handling og lesning deler geometri |
| `PRIVATE KEY EXPORT` i overskriftens høyre slot | egen rad under listen | tilstand, ikke handling |
| Batteri i nav-slotten | `PAGE 1/2` | ingen sider; regel 10 |
| Hintlinje `up/down select, ENTER activates` fjernet | — | nav-linjen og markøren sier det |
| Tap-advarsel: ramme i accent + `!`, KEEP forvalgt | prosa | 8.5; drag-som-tapp-feilen 4. sept |
| Scope: én forklaringslinje | fem | 8.5 |
| Feil lesning i accent (`no response`, `not calibrated`) | — | varsel er en aksentbetydning |

## Overlegg

| Endring | Fra | Hvorfor |
|---|---|---|
| Ramme 2 px rule alle fem | accent | 8.4 |
| Oppdagelse: teller `16 REPEATERS` + tommel, 11 rader | 11 av 16 uten hint | 8.6 |
| Oppdagelse tom: tre linjer med grunn og `ENTER: retry` | — | regel 7 |
| Repeater: `ENTER: arm` → `ENTER AGAIN TO FIRE` i valgt rad | armert bare som fyll | teksten sier tilstanden, fyllet er allerede brukt til «valgt» |
| Repeater: `auth 27s` teller i overskrift | skjult 30 s-regel | regel 4 |
| Navngi: `9/24` teller, full-tilstand `Watch list full (12)` | — | fylt tilstand |
| Nordisk velger: 12×12 celler, rammer i rule, valgt accent | — | felles knappeform |

## Uendret, med vilje

Nav-linjens geometri og fanesentre · ordmerket (8.10) · natt-paletten · kanal- og navnefarger (8.7) · to rullemotorer (8.9) · oppstartsskjermen · emoji-mapping (8.11) · valgt rad = accent + on_accent · ingen tittellinje.
