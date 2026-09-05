# COMMS — spesifikasjon

Spørsmål: *Samtaler.* Rendering: `renders/comms-{night,day}-2x.png`, fem kanaler: `comms-5ch-*`, samtaleliste: `convlist-*`, nordisk velger: `nordic-*`.

## Fanestripe (y 0–13)

| # | Element | Koordinat | Rolle | Streng |
|---|---|---|---|---|
| 1 | Fane k | boks [x, 1, w, 12], w = len·6 + 8 (+6 hvis ulest); tekst (x+4, 3); neste x = x + w + 4, første x 2 | aktiv: fill accent, tekst on_accent · kanal 1–4: drawRect i kanalfargen, tekst fg · kanal 0 og ≥5: drawRect rule, tekst fg | `Public` `#test` `#lillemesh` `#oslo` `#ski` |
| 1a | Ulest i fane | fillRect [x+w−6, 5, 3, 3] | fg | |
| 1b | Stripe-overflyt | glyf `»` ved (310, 3) når faner til høyre ikke får plass; `«` ved (2, 3) til venstre | fg | stripen vindusrulles fanevis, aktiv fane alltid synlig |

## Historikk (y 16–203, pikselrulling, klippet)

| # | Element | Koordinat | Rolle | Streng |
|---|---|---|---|---|
| 2 | Klokkeslett | text (4, y) | dim | `08:31` |
| 3 | Avsender | text (40, y) | navnefarge (slot = hash mod 12); egen: fg | `Per` · `me` |
| 4 | Høyre slot | textR (314, y) | se tilstander | `9 hops` · `√ 3 hops` · `sending` · `no ack` |
| 5 | Brødtekst | text (4, y+12k), 52 tegn per linje, ordbrytning | fg | |
| 6 | Egen melding | fillRect [0, y−2, 2, 12·(1+linjer)] | **fg** | |
| 7 | Vindu | spor [319, 16, 1, 188], tommel x 318; verdier i piksler | rule / fg | |
| 8 | Skille | fillRect [0, 206, 320, 1] | rule | |
| 9 | Skrivelinje | text (4, 212) `>`; tekst fra x 16; markør `_` etter teksten | fg | |
| 10 | Høyre slot skrivelinje | textR (316, 212) | mid | tom: `ENTER: conversations` · skriver: `24/160` · ≥150: `154/160` i fg |
| 11 | Nav | COMMS aktiv | | |

Høyre slot (4) per tilstand: innkommende `N hops` mid (`0 hops` = direkte, aldri tomt) · egen levert `√ N hops` **ok** · egen sendt, venter `sending` mid · egen uten ack etter 60 s `no ack` accent (natt) / accent_txt (dag) · egen i rom uten ack-mekanisme `sent` mid.

## Samtaleliste (ENTER på tom skrivelinje)

| # | Element | Koordinat | Rolle | Streng |
|---|---|---|---|---|
| 12 | Overskrift | text (2, 2) mid `12 conversations`; textR (316, 2) mid `ENTER: open` | | |
| 13 | Rader | 17 rader fra y 16 (siste topp 208) | | |
| 13a | Ulest | fillRect [2, y+2, 3, 3] | fg / on_accent | |
| 13b | Brikke | fillRect [7, y−2, 2, 12] | kanal 1–4: kanalfarge · kanal 0: ingen · kanal ≥5: rule · kontakt: navnefarge | |
| 13c | Navn | text (11, y), maks 40 tegn | fg / on_accent | |
| 13d | Type | text (258, y) | mid / on_accent | `ROOM` når romserver |
| 13e | Alder | textR (314, y) | fg / on_accent | `3m` `4d` `>99d` `?` |
| 14 | Valgt | fillRect [0, y−2, 316, 12] | accent | |
| 15 | Vindu | spor [319, 16, 1, 210] | | |

Rekkefølge uendret: kanaler, samtaler med historikk, kontakter hørt nylig.

## Tilstander

| Tilstand | Endring |
|---|---|
| Tom kanal | historikk: rad ved y 16 `No messages in #oslo yet.` mid; rad y 28 `Type below; ENTER sends to the channel.` mid |
| Ukjent avsender | navn = 6 første hex av public key i mid, f.eks. `a91f3c` |
| Klokke ikke satt | 2 = `--:--` |
| Full historikk (>188 px) | tommel; ny melding ruller til bunn bare hvis vinduet allerede sto der |
| Melding uten ack | 4 = `no ack` accent; ingen annen flate skifter |
| Fem eller flere kanaler | 1: fane 5+ i rule; 1b når stripen er bredere enn 316 |
| Feil (send avvist, kø full) | 10 = `queue full · wait` i accent/accent_txt til neste tastetrykk |

## Endret fra dagens skjerm

- Egen melding markeres med 2 px `fg`-strek, ikke accent (8.4). Formen alene skiller den; ingen annen rad har en strek.
- Levert har fått en form: `√` (0xFB) foran hoppantallet, i `ok`. I dag skiller glyfen levert fra kanalnavn selv om grønn og kanalfarge 1/2 skulle bli forvekslet (8.3).
- `no ack` er eneste aksentbruk i historikken, og den er et varsel.
- Ulest-prikk i fane og samtaleliste er `fg`, 3×3.
- Kanal 0 og ≥5 rammes i `rule` (8.8).
- Skrivelinjens høyre slot viser tegntelling mens man skriver, hint når den er tom.
- Tommel ved x 318–319 i historikk og samtaleliste; teksten slutter ved 314.
