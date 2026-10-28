// SPDX-License-Identifier: MIT
//
// Dashboard app: loads the stored detections from the device and shows them
// as a list, on a map, and with the device's status, one row per address. It
// keeps them up to date while the page is open and keeps the owner's notes
// on addresses. Anything a detected transmitter could influence (advertised
// names above all) is inserted as text, never as HTML.
'use strict';

(() => {
  const $ = (selector) => document.querySelector(selector);
  const PAGE = 150;  // list rows added per "Show more"
  const POLL_MS = 5000;  // status checks while the page is on screen
  const baseTitle = document.title;

  // ---- Helpers --------------------------------------------------------------

  // Builds an element; children that are strings become text nodes.
  function el(tag, props, ...children) {
    const e = document.createElement(tag);
    for (const [k, v] of Object.entries(props || {})) {
      if (k === 'class') e.className = v;
      else if (k === 'text') e.textContent = v;
      else if (k.startsWith('on')) e.addEventListener(k.slice(2), v);
      else e.setAttribute(k, v);
    }
    for (const c of children) if (c !== null && c !== undefined) e.append(c);
    return e;
  }

  function pref(name, fallback) {
    try {
      const v = localStorage.getItem('fy.' + name);
      return v === null ? fallback : v === '1';
    } catch {
      return fallback;
    }
  }

  function setPref(name, on) {
    try {
      localStorage.setItem('fy.' + name, on ? '1' : '0');
    } catch {
      // Not remembered in this browser.
    }
  }

  const plural = (n, one, many) => `${n} ${n === 1 ? one : many}`;

  function duration(ms) {
    const s = Math.max(0, Math.floor(ms / 1000));
    const h = Math.floor(s / 3600);
    const m = Math.floor(s / 60) % 60;
    if (h) return `${h} h ${String(m).padStart(2, '0')} min`;
    if (m) return `${m} min ${s % 60} s`;
    return `${s} s`;
  }

  // An observation's time: local time from GPS or a dashboard phone's clock,
  // or the time since the detector started when it had neither.
  function when(o, boot) {
    if (o.utc) {
      return new Date(o.utc).toLocaleString([], { dateStyle: 'medium', timeStyle: 'short' });
    }
    return `boot ${boot}, ${duration(o.uptime_ms)} after start`;
  }

  const coords = (loc) => `${loc.lat.toFixed(6)}, ${loc.lon.toFixed(6)}`;

  // The badge already says "Possible"; the exports keep the full type name.
  const typeLabel = (type) => type.replace(/ \(possible\)$/, '');

  function fixText(loc) {
    const fix = loc.state === 'current' ? 'GPS fix' : `stale fix, ${loc.age_s} s old`;
    return loc.hdop ? `${fix}, HDOP ${loc.hdop}` : fix;
  }

  // Requests the page makes on its own (polling and the phone's clock) carry
  // X-FY-Background, so an open page does not keep a Wi-Fi
  // session from ending.
  async function request(path, { background = false, form = null } = {}) {
    const init = { credentials: 'same-origin', cache: 'no-store', headers: {} };
    if (background) init.headers['X-FY-Background'] = '1';
    if (form) {
      init.method = 'POST';
      init.body = new URLSearchParams(form);
    }
    const r = await fetch(path, init);
    if (r.status === 401) {
      location.href = '/login?m=exp';
      throw new Error('login needed');
    }
    if (r.status === 403 && (await r.text()).includes('"password"')) {
      location.href = '/password';
      throw new Error('password change needed');
    }
    if (!r.ok) throw new Error(`HTTP ${r.status}`);
    return r;
  }

  const api = async (path, options) => (await request(path, options)).json();

  // ---- Detections -----------------------------------------------------------

  const state = {
    events: new Map(),  // `${boot}:${encounter}` to its newest snapshot
    notes: new Map(),   // address to { mac, label, ignored, note }
    rows: [],
    byId: new Map(),
    shown: 0,
    selected: null,
    busy: false,
    seq: null,  // where the next update starts (X-FY-Next-Seq)
    // The device history the events belong to ("boot-clears", X-FY-History).
    // When the device reports another one (an erase or a reboot), the events
    // are dropped and loaded again; until then the page holds none.
    history: null,
    stale: false,
    generation: 0,  // responses to requests from an older generation are ignored
    oldestSeq: 0,   // events last updated before this record have left the log
  };

  const LABELS = { confirmed: 'Confirmed', false_positive: 'False positive' };
  const noteFor = (mac) => state.notes.get(mac) || null;
  const isIgnored = (r) => Boolean(noteFor(r.mac)?.ignored);

  // Ignored addresses are left off the list and map unless asked for.
  const activeRows = () => ($('#show-ignored').checked ? state.rows : state.rows.filter((r) => !isIgnored(r)));

  // Adds snapshots to the events, keeping the newest of each. Returns the
  // events that are new or have just become specific matches.
  function mergeEvents(list) {
    const changed = [];
    for (const e of list) {
      const key = `${e.boot}:${e.encounter}`;
      const prev = state.events.get(key);
      if (prev && prev.seq >= e.seq) continue;
      state.events.set(key, e);
      if (!prev || (e.specific && !prev.specific)) changed.push({ e, upgraded: Boolean(prev) });
    }
    return changed;
  }

  // The detector's position at the strongest sighting, preferring current
  // fixes over stale ones.
  function bestPosition(events) {
    let pick = null;
    for (const e of events) {
      for (const o of [e.best, e.first, e.last]) {
        if (!o.loc) continue;
        const better = !pick ||
          (o.loc.state === 'current' && pick.loc.state !== 'current') ||
          (o.loc.state === pick.loc.state && o.rssi > pick.rssi);
        if (better) pick = o;
      }
    }
    return pick;
  }

  // One row per address, holding every detection event of it, newest first.
  // Rows are ordered by their latest update.
  function buildRows() {
    const byMac = new Map();
    for (const e of state.events.values()) {
      let r = byMac.get(e.mac);
      if (!r) {
        r = { id: `d:${e.mac}`, mac: e.mac, events: [], seq: 0 };
        byMac.set(e.mac, r);
      }
      r.events.push(e);
      r.seq = Math.max(r.seq, e.seq);
    }
    const rows = [...byMac.values()].sort((a, b) => b.seq - a.seq);
    for (const r of rows) {
      const ev = r.events.sort((a, b) => b.boot - a.boot || b.encounter - a.encounter);
      const top = ev.find((e) => e.specific) || ev[0];
      r.type = top.type;
      r.vendor = top.vendor || '';  // absent in exports from older firmware
      r.rule = top.rule;
      r.transport = top.transport;
      r.specific = ev.some((e) => e.specific);
      r.name = (ev.find((e) => e.name) || { name: '' }).name;
      r.sightings = ev.reduce((n, e) => n + e.sightings, 0);
      r.count = ev.length;
      r.last = ev[0].last;
      r.lastBoot = ev[0].boot;
      r.first = ev[ev.length - 1].first;
      r.firstBoot = ev[ev.length - 1].boot;
      r.rssiMax = Math.max(...ev.map((e) => e.rssi_max));
      r.pos = bestPosition(ev);
    }
    return rows;
  }

  function visibleRows() {
    const q = $('#search').value.trim().toLowerCase();
    const f = $('#filter').value;
    return activeRows().filter((r) => {
      const note = noteFor(r.mac);
      if (f === 'specific' && !r.specific) return false;
      if (f === 'possible' && r.specific) return false;
      if ((f === 'confirmed' || f === 'false_positive') && (!note || note.label !== f)) return false;
      return !q || [r.mac, r.type, r.name, r.vendor, note ? note.note : '']
        .some((text) => text.toLowerCase().includes(q));
    });
  }

  function noteLine(note) {
    return el('div', { class: 'note-line' },
      note.label ? el('span', { class: `tag ${note.label}`, text: LABELS[note.label] }) : null,
      note.ignored ? el('span', { class: 'tag', text: 'Ignored' }) : null,
      note.note ? el('span', { text: note.note }) : null);
  }

  function rowItem(r) {
    const note = noteFor(r.mac);
    const li = el('li', {
      class: `row${note && note.ignored ? ' ignored' : ''}`, role: 'option', tabindex: '0', 'data-id': r.id,
      'aria-selected': String(state.selected === r.id),
    },
    el('div', null, el('span', { class: `badge ${r.specific ? 'specific' : 'possible'}`, text: r.specific ? 'Specific' : 'Possible' }),
      el('span', { class: 'type', text: typeLabel(r.type) })),
    el('div', { class: 'count' }, plural(r.count, 'detection', 'detections'),
      el('small', { text: plural(r.sightings, 'sighting', 'sightings') })),
    el('div', { class: 'sub', text: r.name ? `${r.mac} · ${r.name}` : r.mac }),
    el('div', {
      class: 'meta',
      text: `${when(r.last, r.lastBoot)} · ${r.rssiMax} dBm${r.pos ? '' : ' · no position'}`,
    }),
    note ? noteLine(note) : null);
    li.addEventListener('click', () => select(r.id));
    li.addEventListener('keydown', (e) => {
      if (e.key === 'Enter' || e.key === ' ') {
        e.preventDefault();
        select(r.id);
      }
    });
    return li;
  }

  function renderSummary(visible) {
    const summary = $('#summary');
    if (!state.rows.length) {
      summary.textContent = 'No detections stored yet.';
      return;
    }
    const active = activeRows();
    const specific = active.filter((r) => r.specific).length;
    const hidden = state.rows.length - active.length;
    const of = visible === active.length ? '' : `${visible} of `;
    summary.textContent = `${of}${plural(active.length, 'address', 'addresses')}: ` +
      `${specific} with a specific match, ${active.length - specific} possible only` +
      (hidden ? `; ${plural(hidden, 'ignored address', 'ignored addresses')} not shown.` : '.');
  }

  // Shows the first page of rows, or at least `keep` rows when redrawing.
  function renderList(reset, keep = 0) {
    const rows = visibleRows();
    const list = $('#rows');
    if (reset) {
      list.replaceChildren();
      state.shown = 0;
    }
    const end = Math.min(rows.length, Math.max(state.shown + PAGE, keep));
    for (let i = state.shown; i < end; i++) list.append(rowItem(rows[i]));
    state.shown = end;
    $('#more').hidden = state.shown >= rows.length;
    renderSummary(rows.length);
  }

  // Makes sure the row is in the list (past "Show more" pages) and visible.
  function revealRow(id) {
    const find = () => $('#rows').querySelector(`[data-id="${CSS.escape(id)}"]`);
    let li = find();
    while (!li && !$('#more').hidden) {
      renderList(false);
      li = find();
    }
    if (li) li.scrollIntoView({ block: 'nearest' });
  }

  function eventItem(e) {
    const pos = bestPosition([e]);
    const span = e.last.uptime_ms - e.first.uptime_ms;
    const parts = [
      e.specific ? 'specific' : 'possible',
      plural(e.sightings, 'sighting', 'sightings'),
      `${e.rssi_min} to ${e.rssi_max} dBm`,
      pos ? coords(pos.loc) : 'no position',
    ];
    if (!e.closed) parts.push('still open');
    return el('li', null,
      el('div', { text: `${when(e.first, e.boot)}, ${duration(span)}` }),
      el('div', { class: 'muted', text: parts.join(' · ') }));
  }

  // A link that opens the phone's maps app with a labelled pin at the
  // detector's position: Apple Maps on iOS, a geo: URI elsewhere. The label
  // drops parentheses, which end a geo: label.
  function mapsLink(r) {
    const lat = r.pos.loc.lat.toFixed(7);
    const lon = r.pos.loc.lon.toFixed(7);
    const label = encodeURIComponent(typeLabel(r.type).replace(/[()]/g, ''));
    const ios = /iPad|iPhone|iPod/.test(navigator.userAgent) ||
      (navigator.platform === 'MacIntel' && navigator.maxTouchPoints > 1);
    if (ios) return `https://maps.apple.com/?ll=${lat},${lon}&q=${label}`;
    return `geo:0,0?q=${lat},${lon}(${label})`;
  }

  function detailInfo(r) {
    const dl = el('dl');
    const item = (k, v) => dl.append(el('dt', { text: k }), el('dd', { text: v }));
    item('Address', r.mac);
    if (r.vendor) item('Vendor', r.vendor);
    if (r.name) item('Name', r.name);
    item('Radio', r.transport === 'ble' ? 'Bluetooth LE' : 'Wi-Fi');
    item('Match', `${r.specific ? 'Specific' : 'Possible'} (${r.rule})`);
    item('Detections', String(r.count));
    item('Sightings', String(r.sightings));
    item('First seen', when(r.first, r.firstBoot));
    item('Last seen', when(r.last, r.lastBoot));
    item('Strongest signal', `${r.rssiMax} dBm`);
    item('Position', r.pos ? `${coords(r.pos.loc)} (${fixText(r.pos.loc)})` : 'None recorded');

    const actions = el('p', { class: 'actions' });
    if (r.pos) {
      actions.append(
        el('button', {
          type: 'button', class: 'plain', text: 'Show on map',
          onclick: () => { showView('map'); map.focus(r.id); },
        }),
        el('a', { class: 'button', text: 'Open in maps app', href: mapsLink(r) }));
    }
    const notes = [
      'Position is the detector\'s own position at the strongest sighting, not the device\'s.',
      r.specific
        ? 'A specific match identifies the kind of device.'
        : 'A possible match rests on a broad identifier, such as a manufacturer prefix, and does not show what the device is.',
      'Every detection of this address is grouped here.',
    ];
    if (r.transport === 'ble') notes.push('Bluetooth devices that change their address appear once for each address.');
    return [el('h3', { text: typeLabel(r.type) }), el('p', { class: 'muted', text: notes.join(' ') }), dl, actions];
  }

  function detailEvents(r) {
    if (r.count < 2) return [];
    const list = el('ul', { class: 'events' });
    const shown = r.events.slice(0, 200);
    for (const e of shown) list.append(eventItem(e));
    const out = [el('h3', { text: 'Detections' }), list];
    if (r.events.length > shown.length) {
      out.push(el('p', { class: 'muted', text: `Newest ${shown.length} of ${r.events.length} shown; the export has all.` }));
    }
    return out;
  }

  // What the device keeps of a note: printable ASCII, at most 63 characters,
  // trimmed.
  const cleanNote = (text) => text.replace(/[^\x20-\x7e]/g, '').trimStart().slice(0, 63).trimEnd();

  // Stores the note on an address ({ mac, label, ignored, note }) on the
  // device, then redraws. Throws a message for the owner on failure.
  async function saveNote(saved, rebuildDetail = false) {
    try {
      await request('/api/notes', { form: { ...saved, ignored: saved.ignored ? '1' : '0' } });
    } catch (err) {
      throw new Error(err.message === 'HTTP 409'
        ? 'Not saved: notes are kept for at most 64 addresses.'
        : `Not saved: ${err.message}`);
    }
    if (saved.label || saved.ignored || saved.note) state.notes.set(saved.mac, saved);
    else state.notes.delete(saved.mac);
    redraw({ rebuildDetail });
    renderIgnored();
  }

  function noteForm(mac) {
    const note = noteFor(mac) || { label: '', ignored: false, note: '' };
    const radio = (value, text) => {
      const input = el('input', { type: 'radio', name: 'label', value });
      input.checked = note.label === value;
      return el('label', { class: 'choice' }, input, ` ${text}`);
    };
    const text = el('input', { type: 'text', name: 'note', maxlength: '63', autocomplete: 'off' });
    text.value = note.note;
    const ignored = el('input', { type: 'checkbox', name: 'ignored' });
    ignored.checked = note.ignored;
    const status = el('span', { class: 'muted', 'aria-live': 'polite' });
    const form = el('form', { class: 'note-form' },
      el('h3', { text: 'Your notes' }),
      el('fieldset', null, el('legend', { text: 'Label' }),
        radio('', 'None'), radio('confirmed', 'Confirmed'), radio('false_positive', 'False positive')),
      el('label', { class: 'field' }, 'Note', text),
      el('label', { class: 'check' }, ignored,
        ' Ignore this address: leave it off the list, map, and exports, with no alerts'),
      el('p', { class: 'actions' }, el('button', { type: 'submit', class: 'plain', text: 'Save' }), status));
    // A false positive is usually not worth seeing again; the owner can
    // still untick Ignore before saving.
    form.addEventListener('change', (ev) => {
      if (ev.target.name === 'label' && ev.target.value === 'false_positive') ignored.checked = true;
    });
    form.addEventListener('submit', async (ev) => {
      ev.preventDefault();
      text.value = cleanNote(text.value);
      status.textContent = 'Saving…';
      try {
        await saveNote({ mac, label: form.elements.label.value, ignored: ignored.checked, note: text.value });
        status.textContent = 'Saved.';
      } catch (err) {
        status.textContent = err.message;
      }
    });
    return form;
  }

  // The details keep their notes form while the same address stays
  // selected, so redraws for live updates do not disturb typing.
  function renderDetail(r, rebuild = false) {
    const box = $('#detail');
    if (rebuild || box.dataset.mac !== r.mac) {
      box.dataset.mac = r.mac;
      box.replaceChildren(
        el('button', { type: 'button', class: 'close', 'aria-label': 'Close details', text: '×', onclick: closeDetail }),
        el('div', { class: 'detail-info' }),
        noteForm(r.mac),
        el('div', { class: 'detail-events' }));
    }
    box.querySelector('.detail-info').replaceChildren(...detailInfo(r));
    box.querySelector('.detail-events').replaceChildren(...detailEvents(r));
    box.hidden = false;
  }

  function closeDetail() {
    state.selected = null;
    for (const li of $('#rows').children) li.setAttribute('aria-selected', 'false');
    const box = $('#detail');
    box.hidden = true;
    delete box.dataset.mac;
    map.highlight(null);
  }

  function select(id) {
    const r = state.byId.get(id);
    if (!r) return;
    state.selected = id;
    for (const li of $('#rows').children) li.setAttribute('aria-selected', String(li.dataset.id === id));
    renderDetail(r);
    map.highlight(id);
  }

  // Rebuilds the rows from the events and redraws what shows them.
  function redraw({ refit = false, rebuildDetail = false } = {}) {
    state.rows = buildRows();
    state.byId = new Map(state.rows.map((r) => [r.id, r]));
    renderList(true, state.shown);
    map.setDots(activeRows(), refit);
    const r = state.byId.get(state.selected);
    if (r) renderDetail(r, rebuildDetail);
    else if (state.selected) closeDetail();
  }

  // Loads every stored detection. Opening the page and Refresh count as the
  // owner using the dashboard; the page's own reloads (a retry, or after an
  // erase or reboot) are background requests.
  async function load({ background = false } = {}) {
    if (state.busy) return;
    state.busy = true;
    const generation = state.generation;
    $('#summary').textContent = 'Loading detections…';
    try {
      const [r, notes] = await Promise.all([
        request('/api/detections', { background }), api('/api/notes', { background })]);
      const list = await r.json();
      if (generation !== state.generation) return;  // overtaken by an invalidation
      state.events = new Map();
      mergeEvents(list);
      state.notes = new Map(notes.map((n) => [n.mac, n]));
      state.seq = Number(r.headers.get('X-FY-Next-Seq')) || 0;
      state.history = r.headers.get('X-FY-History');
      state.oldestSeq = Number(r.headers.get('X-FY-Oldest-Seq')) || 0;
      state.stale = false;
      redraw({ refit: true, rebuildDetail: true });
      renderIgnored();
    } catch (err) {
      // Includes a response the device cut short because records left its
      // log meanwhile. Retried at the next check.
      if (generation === state.generation) state.stale = true;
      $('#summary').textContent = `Could not load detections (${err.message}). Trying again…`;
    } finally {
      state.busy = false;
    }
  }

  // The device's history changed (an erase or a reboot): nothing loaded
  // before may be shown or exported as current, so it is dropped at once and
  // loaded again; responses still in flight are ignored.
  function invalidate() {
    state.generation++;
    state.stale = true;
    state.seq = null;
    state.history = null;
    state.events = new Map();
    redraw();
    closeDetail();
    $('#summary').textContent = 'The detector\'s stored history changed. Loading it again…';
  }

  // Drops events whose newest snapshot has left the log as it wrapped.
  function prune(oldestSeq) {
    if (!oldestSeq || oldestSeq <= state.oldestSeq) return;
    state.oldestSeq = oldestSeq;
    let removed = 0;
    for (const [key, e] of state.events) {
      if (e.seq < oldestSeq) {
        state.events.delete(key);
        removed++;
      }
    }
    if (removed) redraw();
  }

  // Fetches the encounters written since the last load or update.
  async function update() {
    if (state.busy) return;
    state.busy = true;
    const generation = state.generation;
    try {
      let r;
      let list;
      try {
        r = await request(`/api/detections?since=${state.seq}`, { background: true });
        list = await r.json();
      } catch {
        // Cut short (records left the log meanwhile) or lost: load everything
        // again at the next check rather than merge a partial answer.
        if (generation === state.generation) state.stale = true;
        return;
      }
      if (generation !== state.generation) return;
      if (r.headers.get('X-FY-History') !== state.history) {
        invalidate();  // erased or rebooted between polls
        return;
      }
      state.seq = Number(r.headers.get('X-FY-Next-Seq')) || state.seq;
      const changed = mergeEvents(list);
      if (list.length) redraw();
      prune(Number(r.headers.get('X-FY-Oldest-Seq')) || 0);
      announce(changed);
    } finally {
      state.busy = false;
    }
  }

  // The Device tab's list of ignored addresses, each with a way back.
  function renderIgnored() {
    const list = $('#ignored');
    const notes = [...state.notes.values()].filter((n) => n.ignored).sort((a, b) => a.mac.localeCompare(b.mac));
    $('#ignored-none').hidden = notes.length > 0;
    list.replaceChildren(...notes.map((n) => {
      const r = state.byId.get(`d:${n.mac}`);
      const status = el('span', { class: 'muted', 'aria-live': 'polite' });
      const button = el('button', { type: 'button', class: 'plain', text: 'Stop ignoring' });
      button.addEventListener('click', async () => {
        button.disabled = true;
        try {
          await saveNote({ ...n, ignored: false }, true);
        } catch (err) {
          status.textContent = err.message;
          button.disabled = false;
        }
      });
      return el('li', null,
        el('div', { class: 'mono', text: n.mac }),
        el('div', { class: 'muted', text: r ? `${r.specific ? 'Specific' : 'Possible'}: ${typeLabel(r.type)}, ${plural(r.count, 'detection', 'detections')}` : 'No stored detections' }),
        n.label || n.note ? noteLine({ ...n, ignored: false }) : null,
        el('p', { class: 'actions' }, button, status));
    }));
  }

  // ---- Alerts ---------------------------------------------------------------

  let unseen = 0;

  function dismissAlert() {
    $('#alert').hidden = true;
    unseen = 0;
    document.title = baseTitle;
  }

  // A banner, a vibration, and a count in the title for new detections and
  // ones that have just become specific matches, except for addresses the
  // owner ignores or marked as false positives.
  function announce(changed) {
    const items = changed.filter(({ e }) => {
      const note = noteFor(e.mac);
      return !(note && (note.ignored || note.label === 'false_positive'));
    });
    if (!items.length) return;
    unseen += items.length;
    const { e, upgraded } = items.find((x) => x.e.specific) || items[0];
    const more = items.length > 1 ? ` and ${plural(items.length - 1, 'other', 'others')}` : '';
    $('#alert').replaceChildren(
      el('span', {
        text: `${upgraded ? 'Now specific' : 'New'}: ${e.specific ? '' : 'possible '}${typeLabel(e.type)}, ${e.mac}${more}`,
      }),
      el('button', {
        type: 'button', text: 'Show',
        onclick: () => {
          dismissAlert();
          showView('list');
          select(`d:${e.mac}`);
          revealRow(`d:${e.mac}`);
        },
      }),
      el('button', { type: 'button', 'aria-label': 'Dismiss', text: '×', onclick: dismissAlert }));
    $('#alert').hidden = false;
    document.title = `(${unseen}) ${baseTitle}`;
    if (pref('vibrate', true) && navigator.vibrate) navigator.vibrate([200, 100, 200]);
  }

  // ---- GPX ------------------------------------------------------------------

  // Text for XML: escaped, without the control characters XML 1.0 forbids.
  const xmlText = (text) => String(text)
    .replace(/[\u0000-\u0008\u000b\u000c\u000e-\u001f]/g, '')
    .replace(/[<>&"']/g, (c) => ({ '<': '&lt;', '>': '&gt;', '&': '&amp;', '"': '&quot;', "'": '&apos;' })[c]);

  // One waypoint per address that has a position, as GPX 1.1. The OsmAnd
  // colour extension makes the pins red there; other apps ignore it.
  function buildGpx(rows) {
    const out = [
      '<?xml version="1.0" encoding="UTF-8"?>',
      '<gpx version="1.1" creator="Limier dashboard" xmlns="http://www.topografix.com/GPX/1/1" xmlns:osmand="https://osmand.net">',
      `<metadata><name>Limier detections</name><time>${new Date().toISOString()}</time></metadata>`,
    ];
    for (const r of rows) {
      if (!r.pos) continue;
      const note = noteFor(r.mac);
      const name = `${r.specific ? '' : 'Possible '}${typeLabel(r.type)}`;
      const desc = [
        r.name ? `${r.mac} (${r.name})` : r.mac,
        r.vendor ? `vendor ${r.vendor}` : '',
        `${plural(r.count, 'detection', 'detections')}, ${plural(r.sightings, 'sighting', 'sightings')}`,
        `last seen ${when(r.last, r.lastBoot)}`,
        `strongest ${r.rssiMax} dBm`,
        `${r.specific ? 'specific' : 'possible'} match (${r.rule})`,
        note && note.label ? `your label: ${LABELS[note.label].toLowerCase()}` : '',
        note && note.note ? `your note: ${note.note}` : '',
        `detector's position at the strongest sighting, ${fixText(r.pos.loc)}`,
      ].filter(Boolean).join('; ');
      out.push(
        `<wpt lat="${r.pos.loc.lat.toFixed(7)}" lon="${r.pos.loc.lon.toFixed(7)}">` +
        (r.pos.utc ? `<time>${r.pos.utc}</time>` : '') +
        `<name>${xmlText(name)}</name><desc>${xmlText(desc)}</desc>` +
        `<type>${r.specific ? 'specific' : 'possible'}</type>` +
        '<extensions><osmand:color>#e53935</osmand:color></extensions></wpt>');
    }
    out.push('</gpx>', '');
    return out.join('\n');
  }

  function downloadGpx() {
    const note = $('#gpx-note');
    if (state.stale) {
      note.textContent = 'The detections are being loaded again from the detector. Try again in a moment.';
      return;
    }
    const rows = $('#export-ignored').checked ? state.rows : state.rows.filter((r) => !isIgnored(r));
    const placed = rows.filter((r) => r.pos).length;
    if (!placed) {
      note.textContent = rows.length
        ? 'No detection has a recorded position yet.'
        : 'No detections loaded yet.';
      return;
    }
    const url = URL.createObjectURL(new Blob([buildGpx(rows)], { type: 'application/gpx+xml' }));
    const a = el('a', { href: url, download: `limier-detections-${localDate()}.gpx` });
    document.body.append(a);
    a.click();
    a.remove();
    setTimeout(() => URL.revokeObjectURL(url), 30000);
    const missing = rows.length - placed;
    note.textContent = `${plural(placed, 'pin', 'pins')}` +
      (missing ? `; ${plural(missing, 'address has', 'addresses have')} no position and ${missing === 1 ? 'is' : 'are'} left out.` : '.');
  }

  // This phone's date as YYYY-MM-DD, for download file names.
  function localDate() {
    const d = new Date();
    const two = (n) => String(n).padStart(2, '0');
    return `${d.getFullYear()}-${two(d.getMonth() + 1)}-${two(d.getDate())}`;
  }

  // The device names the files after the date sent with the request.
  function exportLinks() {
    const q = new URLSearchParams({ date: localDate() });
    if ($('#export-ignored').checked) q.set('ignored', '1');
    $('#csv').href = `/export/detections.csv?${q}`;
    $('#json').href = `/export/detections.json?${q}`;
  }

  // ---- This phone's clock ---------------------------------------------------

  // For detections the detector cannot time from GPS.
  function sendPhoneTime() {
    request('/api/phone', { background: true, form: { t: String(Date.now()) } }).catch(() => {});
  }

  // ---- Status and live updates ----------------------------------------------

  // What the detector is actually doing, from its own capture state.
  function detectionText(s) {
    const c = s.capture;
    if (!c) return s.detection_paused ? 'Paused while the Wi-Fi dashboard is on' : 'Running';
    if (c.wifi && c.ble) return 'Running (Wi-Fi and Bluetooth)';
    if (c.wifi || c.ble) return c.wifi ? 'Wi-Fi only (Bluetooth stopped)' : 'Bluetooth only (Wi-Fi stopped)';
    return s.detection_paused ? 'Paused while the Wi-Fi dashboard is on' : 'Stopped (standby, or capture did not start)';
  }

  function renderStatus(s) {
    const rows = [
      ['Board', s.model || s.board],
      ['Firmware', `Limier ${s.firmware}`],
      ['Dashboard', s.dashboard === 'wifi' ? `Wi-Fi (ends after ${s.idle_minutes} min without use)` : 'USB'],
      ['Detection', detectionText(s)],
      ['Battery', s.battery ? `${s.battery.percent}% (${(s.battery.mv / 1000).toFixed(2)} V)` : 'Not measured'],
      ['GPS', !s.gps ? 'None' : s.gps.fix
        ? `${s.gps.fix_type === 3 ? '3D' : '2D'} fix, ${s.gps.satellites} satellites` : 'No fix'],
    ];
    if (s.phone) rows.push(['Phone clock', s.phone.time ? 'In use for detections without GPS time' : 'Not set']);
    rows.push(
      ['Matches since boot', `${s.matches.reported} (${s.matches.specific} specific)`],
      ['Storage', s.storage
        ? `${s.storage.percent}% used (${s.storage.used} of ${s.storage.capacity} records)` : 'Unavailable'],
      ['Uptime', duration(s.uptime_s * 1000)]);
    if (s.passkey_temporary) rows.push(['Wi-Fi passkey', 'Still the generated one shown on the device']);
    $('#status').replaceChildren(...rows.flatMap(([k, v]) => [el('dt', { text: k }), el('dd', { text: v })]));
    $('#about').textContent = `Limier ${s.firmware} \u00b7 ${s.model || s.board}`;
  }

  async function loadStatus() {
    try {
      renderStatus(await api('/api/status'));
    } catch (err) {
      $('#status').replaceChildren(el('dt', { text: 'Status' }), el('dd', { text: `Could not load: ${err.message}` }));
    }
  }

  let pollTimer = 0;
  const schedulePoll = (ms) => {
    clearTimeout(pollTimer);
    pollTimer = setTimeout(poll, ms);
  };

  // Checks the device's status and fetches new records when there are any.
  // Paused while the page is hidden.
  async function poll() {
    try {
      const s = await api('/api/status', { background: true });
      $('#offline').hidden = true;
      if (!$('#view-device').hidden) renderStatus(s);
      if (s.phone && !s.phone.time) sendPhoneTime();
      const h = s.history;
      if (h && state.history !== null && `${h.boot}-${h.clears}` !== state.history) invalidate();
      if (state.stale) {
        await load({ background: true });  // one at a time; a failed one is retried next poll
      } else if (state.seq !== null && s.record_seq !== undefined && s.record_seq !== state.seq) {
        if (s.record_seq < state.seq) await load({ background: true });  // the log was replaced
        else await update();
      }
      if (h && !state.stale) prune(h.oldest_seq);
    } catch {
      $('#offline').hidden = false;
    } finally {
      if (!document.hidden) schedulePoll(POLL_MS);
    }
  }

  // ---- Map ------------------------------------------------------------------

  const TILE = 256;
  const MIN_ZOOM = 1;
  const MAX_ZOOM = 19;
  const worldSize = (z) => TILE * 2 ** z;

  // Web Mercator: degrees to world pixels at zoom z.
  function project(lat, lon, z) {
    const s = worldSize(z);
    const clamped = Math.max(-85.0511, Math.min(85.0511, lat));
    const sin = Math.sin(clamped * Math.PI / 180);
    return {
      x: (lon + 180) / 360 * s,
      y: (0.5 - Math.log((1 + sin) / (1 - sin)) / (4 * Math.PI)) * s,
    };
  }

  // The detector's own positions with a current fix, from every sighting
  // recorded, in time order (boot sessions are numbered in order). A gap of
  // more than kTrackGapMs, or a new boot, starts a new segment. Points are
  // stored as Web Mercator fractions of the world (0-1).
  const kTrackGapMs = 10 * 60 * 1000;

  function buildTrack(rows) {
    const points = [];
    for (const r of rows) {
      for (const e of r.events) {
        for (const o of [e.first, e.best, e.last]) {
          if (o.loc && o.loc.state === 'current') points.push({ boot: e.boot, t: o.uptime_ms, loc: o.loc });
        }
      }
    }
    points.sort((a, b) => a.boot - b.boot || a.t - b.t);
    const segments = [];
    let segment = null;
    let prev = null;
    for (const p of points) {
      if (prev && prev.boot === p.boot && prev.t === p.t) continue;  // same sighting twice
      if (!prev || prev.boot !== p.boot || p.t - prev.t > kTrackGapMs) {
        segment = [];
        segments.push(segment);
      }
      const w = project(p.loc.lat, p.loc.lon, 0);
      segment.push(w.x / TILE, w.y / TILE);
      prev = p;
    }
    return segments;
  }

  // A distance that is 1, 2, or 5 times a power of ten.
  function roundDistance(m) {
    const p = 10 ** Math.floor(Math.log10(m));
    const f = m / p;
    return (f >= 5 ? 5 : f >= 2 ? 2 : 1) * p;
  }

  // A small slippy map: a plain grid with a red dot per detection that has a
  // position, and optionally the detector's track. No map tiles: a phone on
  // the detector's network has no internet path to fetch them.
  class SlippyMap {
    constructor(root) {
      this.root = root;
      this.tilesPane = root.querySelector('.map-tiles');
      this.dotsPane = root.querySelector('.map-dots');
      this.trackCanvas = root.querySelector('.map-track');
      this.scale = root.querySelector('.map-scale');
      this.popup = $('#map-popup');
      this.track = [];
      this.showTrack = pref('track', false);
      this.zoom = 2;
      this.cx = worldSize(2) / 2;  // view centre in world pixels
      this.cy = worldSize(2) / 2;
      this.tiles = new Map();
      this.dots = [];
      this.selected = null;
      this.fitted = false;
      this.visible = false;
      this.pointers = new Map();
      this.wheel = 0;
      this.onSelect = () => {};
      this.wire();
    }

    get width() { return this.root.clientWidth; }
    get height() { return this.root.clientHeight; }

    local(e) {
      const r = this.root.getBoundingClientRect();
      return { x: e.clientX - r.left, y: e.clientY - r.top };
    }

    wire() {
      const root = this.root;
      root.addEventListener('pointerdown', (e) => {
        if (e.target.closest('.map-zoom, .map-popup')) return;
        root.setPointerCapture(e.pointerId);
        this.pointers.set(e.pointerId, { x: e.clientX, y: e.clientY });
        const dot = e.target.closest('.dot');
        this.down = { x: e.clientX, y: e.clientY, dot: dot ? dot.dataset.id : null };
        this.moved = false;
        if (this.pointers.size === 2) this.pinch = this.pinchDistance();
      });
      root.addEventListener('pointermove', (e) => {
        const p = this.pointers.get(e.pointerId);
        if (!p) return;
        if (this.pointers.size === 1) {
          this.cx -= e.clientX - p.x;
          this.cy -= e.clientY - p.y;
          if (Math.abs(e.clientX - this.down.x) + Math.abs(e.clientY - this.down.y) > 6) this.moved = true;
          this.render();
        }
        p.x = e.clientX;
        p.y = e.clientY;
        if (this.pointers.size === 2) {
          this.moved = true;
          const d = this.pinchDistance();
          const ratio = d / this.pinch;
          if (ratio > 1.6 || ratio < 0.625) {
            const c = this.pinchCentre();
            this.zoomBy(ratio > 1 ? 1 : -1, c.x, c.y);
            this.pinch = d;
          }
        }
      });
      const end = (e) => {
        if (!this.pointers.delete(e.pointerId)) return;
        if (this.pointers.size === 0 && !this.moved && this.down) {
          if (this.down.dot) this.onSelect(this.down.dot);
          else this.closePopup();
        }
      };
      root.addEventListener('pointerup', end);
      root.addEventListener('pointercancel', end);
      root.addEventListener('wheel', (e) => {
        e.preventDefault();
        this.wheel += e.deltaY;
        if (Math.abs(this.wheel) < 60) return;
        const p = this.local(e);
        this.zoomBy(this.wheel < 0 ? 1 : -1, p.x, p.y);
        this.wheel = 0;
      }, { passive: false });
      root.addEventListener('dblclick', (e) => {
        const p = this.local(e);
        this.zoomBy(1, p.x, p.y);
      });
      root.querySelector('.map-zoom').addEventListener('click', (e) => {
        const b = e.target.closest('button');
        if (!b) return;
        if (b.dataset.zoom === 'fit') this.fit();
        else this.zoomBy(Number(b.dataset.zoom), this.width / 2, this.height / 2);
      });
      // A view shown while the page had no size gets its fit once it has one.
      window.addEventListener('resize', () => (this.visible && !this.fitted ? this.fit() : this.render()));
    }

    pinchDistance() {
      const [a, b] = [...this.pointers.values()];
      return Math.hypot(a.x - b.x, a.y - b.y) || 1;
    }

    pinchCentre() {
      const [a, b] = [...this.pointers.values()];
      const r = this.root.getBoundingClientRect();
      return { x: (a.x + b.x) / 2 - r.left, y: (a.y + b.y) / 2 - r.top };
    }

    setTrackVisible(on) {
      this.showTrack = on;
      setPref('track', on);
      this.render();
    }

    // Places a dot per row with a position. A redraw for live updates keeps
    // the view unless the map had no dots before.
    setDots(rows, refit = true) {
      const hadDots = this.dots.length > 0;
      this.track = buildTrack(rows);
      this.dots = rows.filter((r) => r.pos).map((r) => ({
        id: r.id, row: r, lat: r.pos.loc.lat, lon: r.pos.loc.lon, stale: r.pos.loc.state !== 'current',
      }));
      // Each dot is its own element, ready to become draggable when the
      // owner can correct a position.
      this.dotsPane.replaceChildren(...this.dots.map((d) => {
        const cls = `dot${d.stale ? ' stale' : ''}${isIgnored(d.row) ? ' ignored' : ''}`;
        d.el = el('div', { class: cls, 'data-id': d.id, title: typeLabel(d.row.type) });
        return d.el;
      }));
      const missing = rows.length - this.dots.length;
      $('#map-note').textContent = missing
        ? `${plural(missing, 'address has', 'addresses have')} no recorded position and ${missing === 1 ? 'is' : 'are'} not on the map.`
        : '';
      if (refit || !hadDots) {
        this.fitted = false;
        if (this.visible) this.fit();
      } else {
        this.render();
      }
      this.highlight(this.selected);
    }

    show() {
      this.visible = true;
      if (!this.fitted) this.fit();
      else this.render();
    }

    hide() { this.visible = false; }

    setCentre(lat, lon, z) {
      this.zoom = z;
      const p = project(lat, lon, z);
      this.cx = p.x;
      this.cy = p.y;
      this.render();
    }

    fit() {
      if (!this.width) return;  // not displayed yet
      this.fitted = true;
      if (!this.dots.length) {
        // Close in, as for detections: zoomed out to the whole world, the
        // scale bar swings with latitude as the map is dragged.
        this.setCentre(30, 0, 12);
        return;
      }
      const lats = this.dots.map((d) => d.lat);
      const lons = this.dots.map((d) => d.lon);
      const north = Math.max(...lats), south = Math.min(...lats);
      const west = Math.min(...lons), east = Math.max(...lons);
      let z = 17;
      for (; z > MIN_ZOOM; z--) {
        const a = project(north, west, z);
        const b = project(south, east, z);
        if (b.x - a.x < this.width - 60 && b.y - a.y < this.height - 60) break;
      }
      const a = project(north, west, z);
      const b = project(south, east, z);
      this.zoom = z;
      this.cx = (a.x + b.x) / 2;
      this.cy = (a.y + b.y) / 2;
      this.render();
    }

    zoomBy(delta, px, py) {
      const z = Math.max(MIN_ZOOM, Math.min(MAX_ZOOM, this.zoom + delta));
      if (z === this.zoom) return;
      const k = 2 ** (z - this.zoom);
      const ox = px - this.width / 2;
      const oy = py - this.height / 2;
      this.cx = (this.cx + ox) * k - ox;  // keep the point under the pointer still
      this.cy = (this.cy + oy) * k - oy;
      this.zoom = z;
      this.render();
    }

    render() {
      const w = this.width;
      const h = this.height;
      if (!w || !h) return;
      const n = 2 ** this.zoom;
      this.cy = Math.max(0, Math.min(worldSize(this.zoom), this.cy));
      const left = this.cx - w / 2;
      const top = this.cy - h / 2;
      const want = new Set();
      const y0 = Math.max(0, Math.floor(top / TILE));
      const y1 = Math.min(n - 1, Math.floor((top + h) / TILE));
      for (let ty = y0; ty <= y1; ty++) {
        for (let tx = Math.floor(left / TILE); tx <= Math.floor((left + w) / TILE); tx++) {
          const key = `${this.zoom}/${tx}/${ty}`;
          want.add(key);
          let t = this.tiles.get(key);
          if (!t) {
            t = el('div', { class: 'grid' });
            this.tiles.set(key, t);
            this.tilesPane.append(t);
          }
          t.style.transform = `translate(${Math.round(tx * TILE - left)}px, ${Math.round(ty * TILE - top)}px)`;
        }
      }
      for (const [key, t] of this.tiles) {
        if (!want.has(key)) {
          t.remove();
          this.tiles.delete(key);
        }
      }
      const s = worldSize(this.zoom);
      for (const d of this.dots) {
        const p = project(d.lat, d.lon, this.zoom);
        const x = p.x - Math.round((p.x - this.cx) / s) * s - left;  // nearest world copy
        d.el.style.transform = `translate(${Math.round(x)}px, ${Math.round(p.y - top)}px)`;
      }
      this.renderTrack(w, h, left, top);
      this.renderScale(w);
    }

    renderTrack(w, h, left, top) {
      const canvas = this.trackCanvas;
      const ratio = window.devicePixelRatio || 1;
      if (canvas.width !== Math.round(w * ratio) || canvas.height !== Math.round(h * ratio)) {
        canvas.width = Math.round(w * ratio);
        canvas.height = Math.round(h * ratio);
      }
      const ctx = canvas.getContext('2d');
      ctx.setTransform(ratio, 0, 0, ratio, 0, 0);
      ctx.clearRect(0, 0, w, h);
      if (!this.showTrack || !this.track.length) return;
      const size = worldSize(this.zoom);  // track points are fractions of the world
      ctx.strokeStyle = 'rgba(30, 136, 229, 0.85)';
      ctx.fillStyle = 'rgba(30, 136, 229, 0.85)';
      ctx.lineWidth = 3;
      ctx.lineJoin = 'round';
      ctx.lineCap = 'round';
      for (const seg of this.track) {
        const x0 = seg[0] * size - left;
        const y0 = seg[1] * size - top;
        if (seg.length === 2) {  // a lone position
          ctx.beginPath();
          ctx.arc(x0, y0, 2.5, 0, 2 * Math.PI);
          ctx.fill();
          continue;
        }
        ctx.beginPath();
        ctx.moveTo(x0, y0);
        for (let i = 2; i < seg.length; i += 2) ctx.lineTo(seg[i] * size - left, seg[i + 1] * size - top);
        ctx.stroke();
      }
    }

    // A scale bar for the view's centre latitude, at most 120 px long.
    renderScale(w) {
      const s = worldSize(this.zoom);
      const lat = Math.atan(Math.sinh(Math.PI * (1 - 2 * this.cy / s))) * 180 / Math.PI;
      const metresPerPixel = 40075016.686 * Math.cos(lat * Math.PI / 180) / s;
      const metres = roundDistance(metresPerPixel * Math.min(120, w / 3));
      this.scale.style.width = `${Math.round(metres / metresPerPixel)}px`;
      this.scale.firstChild.textContent = metres >= 1000 ? `${metres / 1000} km` : `${metres} m`;
    }

    highlight(id) {
      this.selected = id;
      for (const d of this.dots) d.el.classList.toggle('selected', d.id === id);
      if (!id) this.closePopup();
    }

    focus(id) {
      const d = this.dots.find((x) => x.id === id);
      if (!d) return;
      this.fitted = true;
      this.setCentre(d.lat, d.lon, Math.max(this.zoom, 16));
      this.highlight(id);
      this.openPopup(d);
    }

    openPopup(d) {
      const r = d.row;
      this.popup.replaceChildren(
        el('strong', { text: `${r.specific ? 'Specific' : 'Possible'}: ${typeLabel(r.type)}` }),
        el('div', { text: r.name ? `${r.mac} · ${r.name}` : r.mac }),
        el('div', {
          class: 'muted',
          text: `${plural(r.count, 'detection', 'detections')} · last ${when(r.last, r.lastBoot)}`,
        }),
        el('button', {
          type: 'button', class: 'plain', text: 'Details',
          onclick: () => { showView('list'); revealRow(r.id); },
        }));
      this.popup.hidden = false;
    }

    closePopup() { this.popup.hidden = true; }
  }

  // ---- Views ----------------------------------------------------------------

  const map = new SlippyMap($('#map'));
  map.onSelect = (id) => {
    select(id);
    const d = map.dots.find((x) => x.id === id);
    if (d) map.openPopup(d);
  };

  function showView(name) {
    for (const b of document.querySelectorAll('.tabs button')) {
      b.setAttribute('aria-selected', String(b.dataset.view === name));
    }
    for (const v of ['list', 'map', 'export', 'device']) $(`#view-${v}`).hidden = v !== name;
    document.body.classList.toggle('map-shown', name === 'map');
    if (name === 'map') map.show();
    else map.hide();
    keepOffTop();
    if (name === 'device') {
      loadStatus();
      loadSettings();
    }
  }

  // Android's network sign-in window reloads the page on any downward drag
  // while the page is scrolled to the very top, a drag on the map included.
  // Scrolled down a pixel, the page is not at the top, so the map keeps its
  // drags.
  function keepOffTop() {
    if (map.visible && window.scrollY <= 0) window.scrollTo(0, 1);
  }
  window.addEventListener('scroll', keepOffTop, { passive: true });

  document.querySelector('.tabs').addEventListener('click', (e) => {
    const b = e.target.closest('button[data-view]');
    if (b) showView(b.dataset.view);
  });
  $('#search').addEventListener('input', () => renderList(true));
  $('#filter').addEventListener('change', () => renderList(true));
  const showIgnored = $('#show-ignored');
  showIgnored.checked = pref('showIgnored', false);
  showIgnored.addEventListener('change', () => {
    setPref('showIgnored', showIgnored.checked);
    redraw();
  });
  $('#refresh').addEventListener('click', () => load());
  $('#more').addEventListener('click', () => renderList(false));
  const track = $('#track');
  track.checked = map.showTrack;
  track.addEventListener('change', () => map.setTrackVisible(track.checked));
  $('#gpx').addEventListener('click', downloadGpx);
  $('#export-ignored').addEventListener('change', exportLinks);
  // Refreshed as each download starts, in case the date changed meanwhile.
  $('#csv').addEventListener('click', exportLinks);
  $('#json').addEventListener('click', exportLinks);
  const vibrate = $('#vibrate');
  vibrate.checked = pref('vibrate', true);
  vibrate.addEventListener('change', () => setPref('vibrate', vibrate.checked));
  document.addEventListener('keydown', (e) => {
    if (e.key === 'Escape' && !$('#detail').hidden) closeDetail();
  });
  // Back on screen: check for news now.
  document.addEventListener('visibilitychange', () => {
    if (document.hidden) clearTimeout(pollTimer);
    else schedulePoll(0);
  });

  // ---- Device settings ------------------------------------------------------------

  // Upstream's confidence tiers that the detector's rules use, with the
  // buzzer's sound for each (tier 0 has no rule here).
  const TIERS = [
    [4, 'Probe with the Flock fingerprint', 'high rising chirp'],
    [3, 'Wildcard probe or Flock Wi-Fi network', 'rising chirp'],
    [2, 'Transmitter address or Bluetooth signature', 'blip'],
    [1, 'Receiver or network address, or Bluetooth vendor', 'low blip'],
  ];
  let settings = null;
  const settingsNote = (text) => { $('#settings-note').textContent = text; };

  $('#set-tiers').replaceChildren(...TIERS.map(([tier, what, sound]) => el('label', { class: 'check' },
    el('input', { type: 'checkbox', value: String(tier) }), ` ${what} (${sound})`)));

  function renderSettings(s) {
    settings = s;
    for (const r of document.querySelectorAll('input[name="mode"]')) r.checked = r.value === s.mode;
    $('#set-alerts').checked = s.alerts;
    $('#set-buzzer').checked = s.buzzer;
    $('#set-buzzer').disabled = !s.buzzer_fitted;
    $('#set-flash').checked = s.flash;
    $('#set-flash-row').hidden = !s.display;
    $('#no-buzzer').hidden = s.buzzer_fitted;
    for (const box of document.querySelectorAll('#set-tiers input')) {
      box.checked = Boolean((s.tiers >> Number(box.value)) & 1);
      box.disabled = !s.buzzer_fitted;
    }
  }

  async function loadSettings() {
    try {
      renderSettings(await api('/api/settings'));
    } catch (err) {
      settingsNote(`Could not load the settings (${err.message}).`);
    }
  }

  // Each change is saved at once; the device answers with every setting.
  async function changeSettings(form) {
    try {
      renderSettings(await api('/api/settings', { form }));
      settingsNote('Saved.');
    } catch (err) {
      settingsNote(`Not saved (${err.message}).`);
      loadSettings();
    }
  }

  for (const r of document.querySelectorAll('input[name="mode"]')) {
    r.addEventListener('change', () => changeSettings({ mode: r.value }));
  }
  $('#set-alerts').addEventListener('change', (e) => changeSettings({ alerts: e.target.checked ? 1 : 0 }));
  $('#set-buzzer').addEventListener('change', (e) => changeSettings({ buzzer: e.target.checked ? 1 : 0 }));
  $('#set-flash').addEventListener('change', (e) => changeSettings({ flash: e.target.checked ? 1 : 0 }));
  $('#set-tiers').addEventListener('change', (e) => {
    if (!settings) return;
    const bit = 1 << Number(e.target.value);
    changeSettings({ tiers: e.target.checked ? settings.tiers | bit : settings.tiers & ~bit });
  });

  $('#test-alert').addEventListener('click', async () => {
    if (settings && !settings.alerts) {
      settingsNote('Alerts are off.');
      return;
    }
    try {
      await api('/api/test-alert', { form: {} });
      settingsNote('Test alert sent.');
    } catch (err) {
      settingsNote(`Test failed (${err.message}).`);
    }
  });

  $('#save-open').addEventListener('click', async () => {
    settingsNote('Saving…');
    try {
      const r = await api('/api/save', { form: {} });
      settingsNote(r.saved === true ? 'Open detections saved.'
        : r.saved === false ? 'Save failed.' : 'Save not confirmed in time.');
    } catch (err) {
      settingsNote(`Save failed (${err.message}).`);
    }
  });

  const showEraseConfirm = (on) => {
    $('#erase-confirm').hidden = !on;
    $('#erase-buttons').hidden = !on;
    $('#erase').hidden = on;
  };
  $('#erase').addEventListener('click', () => showEraseConfirm(true));
  $('#erase-no').addEventListener('click', () => {
    showEraseConfirm(false);
    settingsNote('Erase cancelled.');
  });
  $('#erase-yes').addEventListener('click', async () => {
    showEraseConfirm(false);
    try {
      await api('/api/erase', { form: { confirm: 'erase' } });
      settingsNote('Erasing. The list empties when the device finishes.');
    } catch (err) {
      settingsNote(`Erase failed (${err.message}).`);
    }
  });

  exportLinks();
  sendPhoneTime();
  load().then(() => schedulePoll(POLL_MS));
})();
