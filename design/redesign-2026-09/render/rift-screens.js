// RIFT redesign renderer. Draws every screen with the panel's real primitives only:
// fillRect, drawRect (1 px outline), 6x8 CP437 text at integer scale. Unit coordinates, 320x240.
(function (root) {
  const PAL = {
    night: { bg: '#000000', bar: '#1A1A1A', fg: '#FFFFFF', mid: '#9A9A9A', dim: '#8A8A8A', rule: '#707070', accent: '#FF4100', on_accent: '#000000', ok: '#39C800', accent_txt: '#FF4100' },
    day:   { bg: '#FFFFFF', bar: '#EFEBEF', fg: '#000000', mid: '#5A5A5A', dim: '#6B6B6B', rule: '#8C8C8C', accent: '#FF4100', on_accent: '#000000', ok: '#2A6400', accent_txt: '#202020' }
  };
  const CH = ['#737D00', '#00864A', '#6361F7', '#D62C84']; // channel colours 1-4 (0x73E0 0x0429 0x631E 0xD170)
  // name colours: slots 0-3 are the channel colours; 4-11 come from RiftLogic.h — three stand-ins from the 1091-value band here
  const NAME = CH.concat(['#A05A00', '#0080A8', '#B03CC8', '#8C6E00']);
  const CP = { '·': 0xFA, '√': 0xFB, '»': 0xAF, '«': 0xAE, '►': 0x10, '◄': 0x11, '▲': 0x1E, '▼': 0x1F, '■': 0xFE, '█': 0xDB, '°': 0xF8, 'å': 0x86, 'ä': 0x84, 'æ': 0x91, 'ø': 0xED, 'Ø': 0xE8, 'Å': 0x8F, 'Ä': 0x8E, 'Æ': 0x92, 'ö': 0x94, 'ü': 0x81 };
  let FONT = null;
  function font() {
    if (FONT) return FONT;
    const hex = root.RIFT_FONT_HEX; FONT = new Uint8Array(hex.length / 2);
    for (let i = 0; i < FONT.length; i++) FONT[i] = parseInt(hex.substr(i * 2, 2), 16);
    // Proposed hand-drawn glyphs (8.11): ø at 0xED (was φ), Ø at 0xE8 (was Φ) — CP437 has neither, Norwegian text needs both
    FONT.set([0x78, 0x64, 0x54, 0x4C, 0x3C], 0xED * 5);
    FONT.set([0x7E, 0x61, 0x51, 0x49, 0x3F], 0xE8 * 5);
    return FONT;
  }

  function G(ctx, mode, s) {
    const P = PAL[mode]; const f = font();
    const g = {
      P, mode, s,
      col(role) { return role[0] === '#' ? role : P[role]; },
      fill(x, y, w, h, role) { ctx.fillStyle = g.col(role); ctx.fillRect(x * s, y * s, w * s, h * s); },
      rect(x, y, w, h, role) { g.fill(x, y, w, 1, role); g.fill(x, y + h - 1, w, 1, role); g.fill(x, y, 1, h, role); g.fill(x + w - 1, y, 1, h, role); },
      rect2(x, y, w, h, role) { g.rect(x, y, w, h, role); g.rect(x + 1, y + 1, w - 2, h - 2, role); },
      glyph(x, y, code, role, sz) {
        ctx.fillStyle = g.col(role);
        for (let c = 0; c < 5; c++) { const b = f[code * 5 + c]; for (let r = 0; r < 7; r++) if (b & (1 << r)) ctx.fillRect((x + c * sz) * s, (y + r * sz) * s, sz * s, sz * s); }
      },
      text(x, y, str, role, sz) {
        sz = sz || 1;
        for (let i = 0; i < str.length; i++) { const ch = str[i]; const code = CP[ch] !== undefined ? CP[ch] : ch.charCodeAt(0); g.glyph(x + i * 6 * sz, y, code < 256 ? code : 63, role, sz); }
        return x + str.length * 6 * sz;
      },
      textR(xEnd, y, str, role, sz) { sz = sz || 1; g.text(xEnd - str.length * 6 * sz, y, str, role, sz); },
      textC(xc, y, str, role, sz) { sz = sz || 1; g.text(xc - Math.floor(str.length * 6 * sz / 2), y, str, role, sz); },
      // reach / activity cell 8x8: full, hollow, dot (4x4 centred)
      cell(x, y, form, role) { if (form === 'full') g.fill(x, y, 8, 8, role); else if (form === 'dot') { g.rect(x, y, 8, 8, role); g.fill(x + 2, y + 2, 4, 4, role); } else g.rect(x, y, 8, 8, role); },
      // RADAR cell 6x8
      cell6(x, y, full, role) { full ? g.fill(x, y, 6, 8, role) : g.rect(x, y, 6, 8, role); },
      // list window pattern: track at x 319, thumb at x 318 w2, only when total > view
      scrollbar(y0, y1, first, view, total) {
        if (total <= view) return; const len = y1 - y0;
        g.fill(319, y0, 1, len, 'rule');
        const th = Math.max(6, Math.round(view / total * len)); const pos = Math.round(first / total * len);
        g.fill(318, y0 + Math.min(pos, len - th), 2, th, 'fg');
      },
      // wordmark: RIFT size 3 with a 4 px seam and 2 px accent along a 12/78 diagonal
      wordmark(x, y, seamX0, seamX1) {
        g.text(x, y, 'RIFT', 'fg', 3);
        for (let sx = seamX0; sx < seamX1; sx++) { const sy = y + 6 + Math.round((sx - x) * 12 / 78); g.fill(sx, sy, 1, 4, 'bg'); g.fill(sx, sy + 1, 1, 2, 'accent'); }
      },
      nav(active, opts) {
        opts = opts || {};
        g.fill(0, 226, 320, 1, 'rule'); g.fill(0, 227, 320, 13, 'bar');
        const tabs = [['RIFT', 20], ['NODES', 81], ['RADAR', 145], ['COMMS', 209], ['SYSTEM', 270]];
        tabs.forEach(([t, xc]) => {
          const w = t.length * 6 + 8, x0 = xc - Math.floor(w / 2);
          if (t === active) {
            if (t === 'RIFT' && mode === 'day') { g.fill(x0, 227, w, 13, 'accent'); g.textC(xc, 230, t, 'on_accent'); }
            else { g.textC(xc, 228, t, mode === 'day' ? 'fg' : 'accent'); g.fill(x0, 237, w, 2, 'accent'); }
          } else g.textC(xc, 228, t, 'mid');
        });
        if (opts.unread) g.fill(240, 231, 3, 3, 'fg');
        const bat = opts.battery == null ? 100 : opts.battery;
        g.textR(318, 228, bat + '%', bat <= 15 ? (mode === 'day' ? 'accent_txt' : 'accent') : 'mid');
        if (bat <= 15 && mode === 'day') { const w = (bat + '%').length * 6; g.fill(318 - w - 4, 227, w + 8, 13, 'accent'); g.textR(318, 230, bat + '%', 'on_accent'); }
      },
      overlay(x, y, w, h) { g.fill(x, y, w, h, 'bg'); g.rect2(x, y, w, h, 'rule'); },
      button(x, y, label, selected) {
        const w = label.length * 6 + 8;
        if (selected) { g.fill(x, y, w, 14, 'accent'); g.text(x + 4, y + 3, label, 'on_accent'); }
        else { g.rect(x, y, w, 14, 'rule'); g.text(x + 4, y + 3, label, 'fg'); }
        return x + w + 6;
      }
    };
    return g;
  }

  const S = {};
  // ---------------- HOME ----------------
  S.home = function (g, o) {
    o = o || {}; const empty = !!o.empty;
    g.fill(0, 0, 320, 240, 'bg');
    g.text(2, 2, 'meshcore.io', 'mid');
    if (empty) g.text(2, 20, 'NO SIGNAL', g.mode === 'day' ? 'accent_txt' : 'accent', 3);
    else g.text(2, 20, 'ACTIVE', 'ok', 3);
    g.wordmark(244, 20, 176, 316);
    g.text(2, 58, empty ? 'LAST RX none since boot' : 'LAST RX 58s AGO', 'fg');
    g.text(2, 70, empty ? 'RX 0 PACKETS' : 'RX 11 PACKETS', 'fg');
    g.text(2, 82, 'USB/BLE CONNECTED', 'mid');
    g.text(2, 100, 'HEARD, LAST 20 MIN', 'mid');
    g.textR(316, 100, empty ? 'RSSI --  SNR --' : 'RSSI -80  SNR -4', 'mid');
    const act = [1, 1, 0, 1, 1, 1, 0, 0, 1, 1, 1, 1, 0, 1, 0, 0, 0, 1, 1, 1];
    for (let i = 0; i < 20; i++) g.cell(2 + i * 12, 112, !empty && act[i] ? 'full' : 'hollow', 'fg');
    g.text(2, 136, empty ? '0/350 STORED · 0 HEARD' : '258/350 STORED · 3 HEARD', 'mid');
    g.text(2, 148, '869.618MHz SF8 22dBm', 'mid');
    if (o.tropo) g.text(2, 160, 'TROPO OPEN · peak 6 hops', 'fg'); else g.text(2, 160, 'TROPO none since boot', 'mid');
    let x = 2; x = g.button(x, 180, 'DISCOVER 0-HOP', true); x = g.button(x, 180, 'ADVERT NEAR', false); g.button(x, 180, 'ADVERT MESH', false);
    g.text(2, 200, 'asks direct neighbours only', 'mid');
    g.nav('RIFT', { unread: true });
  };
  // ---------------- NODES ----------------
  S.nodes = function (g, o) {
    o = o || {}; const empty = !!o.empty;
    g.fill(0, 0, 320, 240, 'bg');
    g.text(2, 2, empty ? '0 NODES' : '3 RECENT · 14 NODES', 'mid'); g.textR(316, 2, 'MAX 10 HOPS', 'mid');
    const bk = [['DIRECT', 1], ['1-2', 5], ['3-5', 4], ['6+', 2], ['NO ROUTE', 2]]; const mx = 5;
    bk.forEach(([l, n], i) => { const x = 2 + i * 63; g.text(x, 14, l, 'mid'); g.rect(x, 24, 58, 4, 'rule'); if (!empty && n) g.fill(x, 24, Math.max(2, Math.round(n / mx * 58)), 4, 'fg'); g.text(x, 30, String(empty ? 0 : n), 'fg'); });
    g.text(2, 44, 'NODE', 'mid'); g.text(140, 44, 'REACH', 'mid'); g.textR(284, 44, 'HOPS', 'mid'); g.textR(314, 44, 'AGE', 'mid');
    if (empty) { g.text(2, 56, 'NO NODES HEARD SINCE BOOT', 'mid'); g.text(2, 68, 'ADVERT NEAR on RIFT asks neighbours', 'mid'); g.nav('NODES'); return; }
    const rows = [
      { n: 'SE-GrensMesh Room...', h: 9, fresh: true, conf: true, age: '3m', sel: true, via: 'via RP2 » Tyri-Asen Solar » RP7', act: 'ENTER: message' },
      { n: 'NO-0787 Holmenkol...', h: 3, fresh: true, conf: true, age: '4m' },
      { n: 'SE-KSD-KAU1 Repeater', h: 10, fresh: true, conf: false, age: '4m' },
      { n: 'Tyri-Asen Solar', h: 2, fresh: false, conf: true, age: '38m' },
      { n: 'LilleMesh Node 3', h: 12, fresh: false, conf: false, age: '2h' },
      { n: 'Oslo-Sentrum RP', h: null, fresh: false, age: '5h' },
      { n: 'Holmenkollen Tower', h: 0, fresh: true, conf: true, age: '1m' },
      { n: 'Nesodden Relay', h: null, fresh: false, age: '2d' },
      { n: 'DK-Room Aalborg', h: 6, fresh: false, conf: false, age: '3d' },
      { n: 'Drammen West', h: 4, fresh: false, conf: true, age: '4d' },
      { n: 'Fornebu RP', h: 1, fresh: false, conf: true, age: '>99d' },
      { n: 'Skien Hub', h: 7, fresh: false, conf: false, age: '?' }
    ];
    let y = 56; const y1 = 224;
    rows.forEach(r => {
      if (y + 8 > y1) return;
      const sel = r.sel; const ink = sel ? 'on_accent' : 'fg';
      if (sel) g.fill(0, y - 2, 316, 12 * 3, 'accent');
      r.fresh ? g.fill(2, y + 1, 5, 5, ink) : g.rect(2, y + 1, 5, 5, ink);
      g.text(10, y, r.n, ink);
      for (let i = 0; i < 10; i++) { const form = r.h == null ? 'hollow' : i < r.h ? (r.conf ? 'full' : 'dot') : 'hollow'; g.cell(140 + i * 12, y, form, ink); }
      g.textR(284, y, r.h == null ? '?' : String(r.h), ink);
      g.textR(314, y, r.age, ink);
      y += 12;
      if (sel) { g.text(10, y, r.via, ink); y += 12; g.text(10, y, r.act, ink); g.textR(314, y, 'RIGHT: control', ink); y += 12; }
    });
    g.scrollbar(56, 226, 0, 14, 16);
    g.nav('NODES');
  };
  // ---------------- RADAR ----------------
  S.radar = function (g, o) {
    o = o || {};
    g.fill(0, 0, 320, 240, 'bg');
    g.text(2, 2, 'WIFI+BLE · 5s SCAN', 'mid');
    if (o.watched !== false) { const l = 'WATCHED: Garmin-4F'; const w = l.length * 6 + 8; g.fill(316 - w, 0, w, 12, 'accent'); g.text(316 - w + 4, 2, l, 'on_accent'); }
    g.text(2, 16, '23', 'fg', 3); g.text(2 + 2 * 18 + 6, 32, 'DEVICES', 'mid');
    const bands = [['CLOSE', '> -60', 6, true], ['MID', '-60/-75', 9, true], ['FAR', '< -75', 30, false]];
    bands.forEach(([l, r, n, full], i) => {
      const y = 48 + i * 12; g.text(2, y, l, 'fg'); g.text(38, y, r, 'dim');
      const cap = 28; for (let k = 0; k < Math.min(n, cap); k++) { if (n > cap && k === cap - 1) g.text(92 + k * 8, y, '»', 'fg'); else g.cell6(92 + k * 8, y, full, 'fg'); }
    });
    g.text(2, 88, 'DEVICE', 'mid'); g.text(254, 88, 'RSSI', 'mid'); g.text(290, 88, 'SEEN', 'mid');
    const rows = [['W', 'Garmin-4F', -48, 'now', true], ['B', 'Tile 3A:0C', -52, 'now', false, true], ['W', 'Telenor-2G-Home', -55, '2s'], ['B', 'Fenix 7', -58, 'now'], ['W', 'iPhone (Kari)', -61, '4s'], ['B', 'unnamed 5C:F3:70', -66, '12s'], ['W', 'ASUS_A0', -70, '8s'], ['B', 'unnamed 71:0A:22', -74, '3s'], ['W', 'JCP-Guest', -79, '40s']];
    rows.forEach(([src, n, rssi, seen, watched, sel], i) => {
      const y = 100 + i * 12; const ink = sel ? 'on_accent' : 'fg';
      if (sel) g.fill(0, y - 2, 316, 12, 'accent');
      g.text(2, y, src, sel ? 'on_accent' : 'mid'); g.text(14, y, n, ink);
      if (watched) g.fill(243, y + 1, 5, 5, ink);
      g.textR(272, y, String(rssi), ink); g.textR(314, y, seen, ink);
    });
    g.scrollbar(100, 226, 0, 9, 23);
    g.text(2, 214, 'ENTER: waterfall  N: name  S: source', 'mid');
    g.nav('RADAR');
  };
  // ---------------- COMMS ----------------
  function tabs(g, active, fifth) {
    const names = ['Public', '#test', '#lillemesh', '#oslo'].concat(fifth ? ['#ski'] : []);
    let x = 2;
    names.forEach((n, i) => {
      const unread = i === 3; const w = n.length * 6 + 8 + (unread ? 6 : 0);
      if (i === active) { g.fill(x, 1, w, 12, 'accent'); g.text(x + 4, 3, n, 'on_accent'); }
      else { g.rect(x, 1, w, 12, i >= 1 && i <= 4 ? CH[i - 1] : 'rule'); g.text(x + 4, 3, n, 'fg'); if (unread) g.fill(x + w - 6, 5, 3, 3, 'fg'); }
      x += w + 4;
    });
  }
  S.comms = function (g, o) {
    o = o || {};
    g.fill(0, 0, 320, 240, 'bg');
    tabs(g, 0, o.fifth);
    const msgs = [
      { t: '00:21', n: 'Tea-PC', c: 4, r: '7 hops', b: ["Don't let the bedbugs bite..."] },
      { t: '00:56', n: 'Tommsen', c: 6, r: '4 hops', b: ['T-Deck +: @[Lb01] denne', 'https://meshwiki.no/meshcore/repeater-oppsett'] },
      { t: '08:31', n: 'Per', c: 1, r: '9 hops', b: ['Godmorgon från SisJön!'] },
      { t: '08:31', n: 'Mstr_m5', c: 5, r: '0 hops', b: ['God morgen'] },
      { t: '08:32', n: 'NO-neero', c: 2, r: '3 hops', b: ['God morgen fra Lillestrøm!'] },
      { t: '08:41', n: 'UF_Tag', c: 7, r: '4 hops', b: ['God morgen fra Oslo!'] },
      { t: '09:02', n: 'me', own: true, r: '√ 3 hops', ok: true, b: ['Morn! Tester ny firmware fra Holmenkollen.'] }
    ];
    // lay out bottom-up from y 202
    const blocks = msgs.map(m => ({ m, h: (1 + m.b.length) * 12 })); let total = blocks.reduce((a, b) => a + b.h, 0);
    let y = 202 - total; if (y < 16) y = 16 - (total - (202 - 16)) ; // clipped top
    blocks.forEach(({ m, h }) => {
      let yy = y;
      if (yy >= 16) {
        g.text(4, yy, m.t, 'dim');
        g.text(40, yy, m.n, m.own ? 'fg' : NAME[m.c % NAME.length]);
        g.textR(314, yy, m.r, m.ok ? 'ok' : 'mid');
      }
      m.b.forEach((line, i) => { const ly = yy + 12 * (i + 1); if (ly >= 16) g.text(4, ly, line, 'fg'); });
      if (m.own) g.fill(0, yy - 2, 2, h, 'fg');
      y += h;
    });
    g.fill(0, 14, 320, 2, 'bg'); g.fill(0, 0, 320, 14, 'bg'); tabs(g, 0, o.fifth); // chrome redraw over clipped top
    g.scrollbar(16, 204, 112, 24, 136);
    g.fill(0, 206, 320, 1, 'rule');
    g.text(4, 212, '>', 'fg'); g.text(16, 212, '_', 'fg'); g.textR(316, 212, 'ENTER: conversations', 'mid');
    g.nav('COMMS', { unread: true });
  };
  S.convlist = function (g) {
    g.fill(0, 0, 320, 240, 'bg');
    g.text(2, 2, '12 conversations', 'mid'); g.textR(316, 2, 'ENTER: open', 'mid');
    const rows = [['Public', null, '', '3m', true], ['#test', CH[0], '', '4d', false], ['#lillemesh', CH[1], '', '1h', true, true], ['#oslo', CH[2], '', '12m'], ['#ski', 'rule', '', '?'], ['SE-GrensMesh Room', NAME[2], 'ROOM', '3m'], ['NO-0787 Holmenkollen', NAME[5], '', '4m'], ['Tea-PC', NAME[4], '', '9h'], ['Tommsen', NAME[6], '', '8h'], ['Per', NAME[1], '', '1h'], ['Mstr_m5', NAME[5], '', '1h'], ['UF_Tag', NAME[7], '', '28m']];
    rows.forEach(([n, chip, room, age, unread, sel], i) => {
      const y = 16 + i * 12; const ink = sel ? 'on_accent' : 'fg';
      if (sel) g.fill(0, y - 2, 316, 12, 'accent');
      if (unread) g.fill(2, y + 2, 3, 3, ink);
      if (chip) g.fill(7, y - 2, 2, 12, chip === 'rule' ? 'rule' : chip);
      g.text(11, y, n, ink); if (room) g.text(258, y, room, sel ? 'on_accent' : 'mid'); g.textR(314, y, age, ink);
    });
    g.scrollbar(16, 226, 0, 17, 12);
    g.nav('COMMS');
  };
  // ---------------- SYSTEM ----------------
  S.system = function (g, o) {
    o = o || {}; const first = o.first || 0;
    g.fill(0, 0, 320, 240, 'bg');
    g.text(2, 2, 'v0.9.2 · UP 3h12m', 'mid'); g.textR(316, 2, 'PRIVATE KEY EXPORT: off', 'mid');
    const items = [
      ['H', 'ACTIONS'], ['A', 'Edit node name', 'NO-0787 Holmenkollen'], ['A', 'Add channel'], ['A', 'Delete channel'], ['A', 'Channel scope'], ['A', 'Path hash size', '2 bytes'], ['A', 'Screen', 'always on'], ['A', 'Alert sound', 'on'], ['A', 'Display', 'night'], ['A', 'Set time', '09:18'],
      ['H', 'DEVICE'], ['R', 'KEYBOARD', 'ok'], ['R', 'TOUCH', '118,204 / 1802,1430'], ['R', 'GPS', 'no fix · 0 sats'], ['R', 'CLOCK', '09:18 from GPS'], ['R', 'EXT POWER', 'usb'], ['R', 'USB SERIAL', 'companion'], ['R', 'BOOT BTN', 'high'],
      ['H', 'MESH'], ['R', 'CONTACTS', '14'], ['R', 'PATH CACHE', '31/64'], ['R', 'TROPO', 'closed'], ['R', 'MSG WAKE', 'on'],
      ['H', 'RUNTIME'], ['R', 'FREE HEAP', '118 kB'], ['R', 'LAST RESET', 'power-on'], ['R', 'BOOT', '1.8 s'], ['R', 'SLOWEST', 'lora init 640 ms'],
      ['H', 'EVENT LOG'], ['R', '09:14', 'advert NO-0787'], ['R', '09:12', 'rx 3 pkt · 1 dup'], ['A', 'View log', '128 lines'], ['A', 'View air log', '11 rx, 0 tx']
    ];
    const view = 17;
    for (let k = 0; k < view; k++) {
      const it = items[first + k]; if (!it) break; const y = 16 + k * 12; const sel = first + k === (o.sel == null ? 3 : o.sel);
      if (it[0] === 'H') { g.text(2, y, it[1], 'mid'); continue; }
      const ink = sel ? 'on_accent' : 'fg';
      if (sel) g.fill(0, y - 2, 316, 12, 'accent');
      g.text(2, y, it[1], ink);
      if (it[2]) g.textR(314, y, it[2], sel ? 'on_accent' : it[0] === 'A' ? 'mid' : 'fg');
      if (it[0] === 'A' && !sel) g.text(308, y, '', ink);
    }
    g.scrollbar(16, 226, first, view, items.length);
    g.nav('SYSTEM');
  };
  S.confirm = function (g) {
    S.system(g, { first: 0, sel: 3 });
    g.overlay(6, 60, 308, 104);
    g.text(10, 66, 'DELETE CHANNEL #test', 'fg');
    g.rect(10, 80, 300, 40, g.mode === 'day' ? 'accent' : 'accent');
    g.text(14, 84, '! CANNOT BE UNDONE', 'fg');
    g.text(14, 96, 'The key and 212 stored messages are erased.', 'fg');
    g.text(14, 108, 'Members keep the channel; you leave it.', 'fg');
    let x = 10; x = g.button(x, 130, 'KEEP', true); g.button(x, 130, 'DELETE', false);
    g.text(10, 150, 'RIGHT then ENTER deletes · DBL-CLICK: cancel', 'mid');
  };
  // ---------------- OVERLAYS ----------------
  S.discover = function (g, o) {
    o = o || {}; S.home(g);
    g.overlay(6, 22, 308, 186);
    g.text(10, 26, o.empty ? 'DISCOVER 0-HOP · NO REPLY' : 'DISCOVER 0-HOP · 16 REPEATERS', 'mid'); g.textR(310, 26, '8s', 'mid');
    if (o.empty) { g.text(10, 50, 'No repeater answered within 8 s.', 'fg'); g.text(10, 62, '0-hop asks direct neighbours only.', 'mid'); g.text(10, 74, 'Try ADVERT NEAR, or move.', 'mid'); g.text(10, 194, 'ENTER: retry · DBL-CLICK: close', 'mid'); return; }
    g.text(10, 38, 'REPEATER', 'mid'); g.text(220, 38, 'RX SNR', 'mid'); g.text(268, 38, 'TX SNR', 'mid');
    const rows = [['Holmenkollen Tower', 11.5, 9.0, true], ['Tyri-Asen Solar', 8.2, 6.5], ['NO-0787 RP2', 5.0, 4.8], ['Fornebu RP', 2.5, null], ['Oslo-Sentrum RP', 1.0, -1.2], ['Drammen West', -2.2, -3.0], ['Nesodden Relay', -3.8, -6.5], ['LilleMesh RP1', -5.0, -4.2], ['Skien Hub', -6.1, null], ['SE-KSD-KAU1', -7.9, -9.0], ['Bærum Relay', -9.5, -11.0]];
    rows.forEach(([n, rx, tx, sel], i) => {
      const y = 50 + i * 12; const ink = sel ? 'on_accent' : 'fg';
      if (sel) g.fill(8, y - 2, 301, 12, 'accent');
      g.text(10, y, n, ink); g.textR(256, y, rx.toFixed(1), ink); g.textR(304, y, tx == null ? '?' : tx.toFixed(1), ink);
    });
    g.scrollbar(50, 182, 0, 11, 16); g.fill(318, 50, 2, 132, 'bg'); g.fill(319, 50, 1, 132, 'bg'); // keep inside box
    g.fill(309, 50, 1, 132, 'rule'); g.fill(308, 50, 2, Math.round(11 / 16 * 132), 'fg');
    g.text(10, 194, 'ENTER: control · DBL-CLICK: close', 'mid');
  };
  S.repeater = function (g, o) {
    o = o || {}; S.nodes(g);
    g.overlay(6, 10, 308, 206);
    g.text(10, 14, 'SE-KSD-KAU1 Repeater · CONTROL', 'mid'); g.textR(310, 14, 'auth 27s', 'mid');
    const st = [['UPTIME', '4d 03h 12m'], ['AIRTIME', '2.1% · DUTY 0.8%'], ['BATT', '4.02 V'], ['RX / TX / DUP', '1203 / 418 / 22'], ['TEMP / HUM', '18.5 °C · 62 %']];
    st.forEach(([k, v], i) => { const y = 26 + i * 12; g.text(10, y, k, 'mid'); g.textR(310, y, v, 'fg'); });
    g.text(10, 90, 'COMMAND', 'mid');
    const menu = ['Status', 'Set clock from mine', 'Advert', 'Reboot', 'Set password'];
    menu.forEach((m, i) => {
      const y = 102 + i * 12; const sel = i === 3;
      if (sel) { g.fill(8, y - 2, 301, 12, 'accent'); g.text(10, y, m, 'on_accent'); g.textR(310, y, o.armed ? 'ENTER AGAIN TO FIRE' : 'ENTER: arm', 'on_accent'); }
      else g.text(10, y, m, 'fg');
    });
    const cli = ['> status', 'uptime 4d03h airtime 2.1%', 'batt 4.02 clock 09:17 ok', '> clock sync', 'ok 09:18'];
    g.fill(8, 162, 301, 1, 'rule');
    cli.forEach((l, i) => g.text(10, 166 + i * 10, l, 'mid'));
  };
  S.preview = function (g) {
    S.home(g);
    g.overlay(6, 40, 308, 110);
    g.text(10, 44, '6 NEW MESSAGES', 'mid'); g.textR(310, 44, '#lillemesh', 'mid');
    const rows = [['08:41', 'UF_Tag', 7, 'God morgen fra Oslo!'], ['08:32', 'NO-neero', 2, 'God morgen fra Lillestrøm!'], ['08:31', 'Mstr_m5', 5, 'God morgen'], ['08:31', 'Per', 1, 'Godmorgon från SisJön!'], ['00:56', 'Tommsen', 6, 'T-Deck +: @[Lb01] denne'], ['00:21', 'Tea-PC', 4, "Don't let the bedbugs bite..."]];
    rows.forEach(([t, n, c, b], i) => { const y = 56 + i * 12; g.text(10, y, t, 'dim'); g.text(46, y, n, NAME[c % NAME.length]); g.text(106, y, b.slice(0, 34), 'fg'); });
    g.text(10, 134, 'ENTER: open COMMS · DBL-CLICK: close', 'mid');
  };
  S.namedevice = function (g) {
    S.radar(g, { watched: false });
    g.overlay(6, 70, 308, 86);
    g.text(10, 74, 'NAME DEVICE · 5C:F3:70:A1:22:0C', 'mid');
    g.text(10, 92, '>', 'fg'); g.text(22, 92, 'Garmin-4F_', 'fg'); g.textR(310, 92, '9/24', 'mid');
    g.text(10, 110, 'Named devices are watched: a lamp on', 'mid'); g.text(10, 122, 'RADAR when they appear.', 'mid');
    g.text(10, 140, 'ENTER: save · DBL-CLICK: cancel', 'mid');
  };
  S.nordic = function (g) {
    S.comms(g);
    g.fill(0, 206, 320, 20, 'bg');
    g.fill(0, 206, 320, 1, 'rule'); g.text(4, 212, '>', 'fg'); g.text(16, 212, 'Hei fra Holmenkollen, K_', 'fg'); g.textR(316, 212, '24/160', 'mid');
    g.overlay(10, 184, 68, 20);
    ['a', 'å', 'ä', 'æ'].forEach((c, i) => { const x = 14 + i * 15; if (i === 1) { g.fill(x, 188, 12, 12, 'accent'); g.text(x + 3, 190, c, 'on_accent'); } else { g.rect(x, 188, 12, 12, 'rule'); g.text(x + 3, 190, c, 'fg'); } });
  };
  S.splash = function (g) {
    g.fill(0, 0, 320, 240, 'bg');
    g.wordmark(32, 78, 0, 160);
    g.text(32, 110, 'R A D I O   I N T E L L I G E N C E', 'mid');
    g.text(32, 122, '&   F I E L D   T E R M I N A L', 'mid');
  };

  root.RIFT = { PAL, CH, NAME, screens: S, draw(ctx, name, mode, scale, opts) { const g = G(ctx, mode, scale); S[name](g, opts); } };
})(typeof window !== 'undefined' ? window : globalThis);
