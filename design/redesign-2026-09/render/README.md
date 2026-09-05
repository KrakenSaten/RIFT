# Renderer

Tegner alle skjermene i redesignet med panelets primitiver (fillRect, drawRect, 6x8 CP437) i enhetskoordinater.

```html
<script src="rift-font.js"></script>
<script src="rift-screens.js"></script>
<script>RIFT.draw(canvas.getContext('2d'), 'nodes', 'night', 2, {});</script>
```

Skjermnavn: home, nodes, radar, comms, convlist, system, confirm, discover, repeater, preview, namedevice, nordic, splash. Modus: night | day. Skala: heltall.
rift-font.js er Adafruit GFX classic 5x7 (BSD) med to foreslåtte glyfer: ø 0xED, Ø 0xE8 (se 00-SYSTEM.md §9).
