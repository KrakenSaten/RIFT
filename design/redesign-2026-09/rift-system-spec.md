# SYSTEM — spesifikasjon

Spørsmål: *Innstillinger, og hva gjør enheten akkurat nå?* Rendering: `renders/system-*`, rullet: `system-scrolled-*`, tap-advarsel: `confirm-*`.

## Struktur: én liste, fem grupper, ingen sider

Side 1 / side 2 og `RIGHT: READINGS` er borte. Alt står i én liste med seksjonsoverskrifter (00-SYSTEM §4.5), vindusmønsteret viser hvor du er. Rekkefølgen er hyppigst brukt først:

`ACTIONS` → `DEVICE` → `MESH` → `RUNTIME` → `EVENT LOG`

| # | Element | Koordinat | Rolle | Streng |
|---|---|---|---|---|
| 1 | Overskrift | text (2, 2) | mid | `v0.9.2 · UP 3h12m` |
| 2 | Overskrift høyre | textR (316, 2) | mid; `on` i accent/accent_txt | `PRIVATE KEY EXPORT: off` |
| 3 | Rader | 17 rader fra y 16 (siste topp 208), pitch 12 | | |
| 3a | Gruppeoverskrift | text (2, y) | mid | `ACTIONS` `DEVICE` `MESH` `RUNTIME` `EVENT LOG` |
| 3b | Handlingsrad | text (2, y) etikett; textR (314, y) nåverdi | fg; verdi mid | `Edit node name` / `NO-0787 Holmenkollen` … |
| 3c | Leserad | text (2, y) etikett; textR (314, y) verdi | fg; verdi fg | `TOUCH` / `118,204 / 1802,1430` |
| 3d | Loggrad | text (2, y) tid; textR (314, y) hendelse | fg | `09:14` / `advert NO-0787` |
| 4 | Valgt rad | fillRect [0, y−2, 316, 12] | accent, alt blekk on_accent | bare 3b og 3d/`View log` kan velges |
| 5 | Vindu | spor [319, 16, 1, 210] | | |
| 6 | Nav | SYSTEM aktiv; **batteri** til høyre som på alle andre | | |

### Radinnhold

ACTIONS: `Edit node name` (verdi: navnet) · `Add channel` · `Delete channel` · `Channel scope` · `Path hash size` (`2 bytes`) · `Screen` (`always on`) · `Alert sound` (`on`) · `Display` (`night`/`day`) · `Set time` (`09:18`)
DEVICE: `KEYBOARD` · `TOUCH` (mappet / rå) · `GPS` (`no fix · 0 sats`) · `CLOCK` (`09:18 from GPS` / `not set`) · `EXT POWER` (`usb`/`none`) · `USB SERIAL` (`companion`/`RESCUE CLI`) · `BOOT BTN` (`high`/`low`)
MESH: `CONTACTS` · `PATH CACHE` (`31/64`) · `TROPO` (`closed`/`OPEN · peak 6`) · `MSG WAKE`
RUNTIME: `FREE HEAP` (`118 kB`) · `LAST RESET` · `BOOT` (`1.8 s`) · `SLOWEST` (`lora init 640 ms`)
EVENT LOG: de to nyeste linjene, så `View log` (`128 lines`) og `View air log` (`11 rx, 0 tx`)

Verdier som kan skifte tilstand (Display, Screen) skrives som ord. Ingen verdi står tom: ukjent = `?`, ikke tilkoblet = `none`.

## Logg og air log

Åpnes i kroppen (ikke overlegg): overskrift text (2, 2) mid `EVENT LOG · 128 lines`, textR `newest last`; 17 rader fra y 16; vindusmønster; `DBL-CLICK` tilbake. Air log: `AIR LOG · 11 rx · 0 tx`, kolonner `TIME` `DIR` `LEN` `RSSI/SNR` `FROM`.

## Underskjerm: kanal-scope (8.5)

Overskrift text (2, 2) mid `CHANNEL SCOPE`, textR (316, 2) mid `region per channel`. Rader fra y 16: kanalnavn (fg) og textR region (`(node default)` i mid når ingen er satt). Valgt rad accent. **Én** forklaringslinje ved y 214 i mid: `Scope limits a channel to one region.` De øvrige fire linjene går til dokumentasjonen. Inntastingsskjermen for region har ingen prosa; feltet og `ENTER: save · DBL-CLICK: cancel`.

## Overlegg: tap-advarsel (8.5)

Brukes av `Delete channel`, `Edit node name` (nøkkelbytte), `Set time` bakover, og repeater-kommandoer som sletter. Boks [6, 60, 308, 104] etter 00-SYSTEM §7.

| Element | Koordinat | Rolle | Streng |
|---|---|---|---|
| Tittel | text (10, 66) | fg | `DELETE CHANNEL #test` |
| Advarselsramme | drawRect [10, 80, 300, 40] | **accent** (varsel) | |
| Linje 1 | text (14, 84) | fg | `! CANNOT BE UNDONE` |
| Linje 2–3 | text (14, 96), (14, 108) | fg | `The key and 212 stored messages are erased.` / `Members keep the channel; you leave it.` |
| Knapper | `KEEP` [10,130,32,14] valgt · `DELETE` [48,130,44,14] | accent/on_accent · rule/fg | |
| Hint | text (10, 150) | mid | `RIGHT then ENTER deletes · DBL-CLICK: cancel` |

Forklaring vs. advarsel: en **advarsel** er innrammet i accent og begynner med `!`; en **forklaring** er `mid`-tekst uten ramme og aldri mer enn én linje. KEEP er forvalgt, så et tapp på 7 px som blir tolket som ENTER ikke sletter.

## Tilstander

| Tilstand | Endring |
|---|---|
| Tom logg | EVENT LOG: én rad `no events since boot` mid; `View log` (`0 lines`) kan fortsatt velges |
| Ukjent | `?` som verdi; `CLOCK not set` |
| Klokke ikke satt | 1 = `v0.9.2 · UP 3h12m` (oppetid er alltid kjent), CLOCK `not set`, `Set time` verdi `--:--` |
| Feil | leserad med feil verdi skrives i accent/accent_txt: `KEYBOARD` / `no response`, `TOUCH` / `not calibrated`; dette er varsel, ikke fremheving |
| Full | alltid: 33 rader, 17 synlige; tommel |
| Armert | se overlays-spec |

## Endret fra dagens skjerm

- To sider → én liste med grupper. Én rullemodell og ett vindusmønster på hele enheten; `PAGE 1/2` og `RIGHT: READINGS` bortfaller, nav-linjen viser batteri.
- Handlinger og lesninger deler kolonne-geometri (etikett x 2, verdi høyrestilt 314): innstillingens nåverdi står der lesningen ville stått.
- `PRIVATE KEY EXPORT` flyttet til overskriftens høyre slot; den er en tilstand, ikke en handling, og var det eneste som sto under listen.
- `up/down select, ENTER activates` fjernet; listen er identisk med alle andre lister, og hintlinjen sa det nav-linjen og markøren allerede sa.
- Tap-advarsel har fått en fast form (ramme i accent + `!`); forklaringer er én `mid`-linje uten ramme.
- Scope-skjermens fem linjer er redusert til én; resten går til dokumentasjonen.
