/* LAN sharing (server / client) for the Video Player UI.
 *
 * Loaded before app.js. Three situations:
 *   1. Local app (https://app.local/)            -> LAN = null; LANSettings adds the "區網分享" settings block.
 *   2. Local connect page (app.local/?connect=1)  -> LAN.mode = 'connect': choose a server (Windows client).
 *   3. Page served by a LAN server (http://host:port/ui/)
 *        -> LAN.mode = 'remote': permission-scoped library, data through HTTP + Server-Sent Events.
 *           Windows client (WebView2, ?native=1): playback by the local core from /stream/file.
 *           Browser (Android phone / TV box / any browser): <video> playing /stream/remux (fMP4),
 *           and on a phone-sized screen the bottom-tab layout ("layout A").
 * app.js calls into this file only through window.LAN / window.LANSettings (see the hooks there).
 */
'use strict';
(function () {
  const params = new URLSearchParams(location.search);
  const nativeHost = window.chrome && window.chrome.webview ? window.chrome.webview : null;
  const onServer = location.protocol === 'http:' && location.pathname.startsWith('/ui/');
  const isConnect = !onServer && params.has('connect');
  const escH = (v) => String(v ?? '').replace(/[&<>"']/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c]));
  const store = {
    get(key, fallback) { try { const v = localStorage.getItem(key); return v == null ? fallback : JSON.parse(v); } catch { return fallback; } },
    set(key, value) { try { localStorage.setItem(key, JSON.stringify(value)); } catch { /* private mode */ } },
  };
  const deliver = (message) => { if (typeof receive === 'function') receive(message); };

  // ================================================================= settings block (local app, server PC)
  // Rows in the settings style (st-*): role and name, then (server) the phone address and PIN, 已配對裝置 › and
  // 使用者與權限 › as second-level pages (LANSettings.sub), and 串流方式 under 進階. Device assignments and user edits apply
  // on their own like every other setting: each change schedules lanUsersSave (debounced, typing waits longer).
  const ICON = {
    trash: '<path d="M4 7h16"></path><path d="M10 11v6M14 11v6"></path><path d="M6 7l1 12a2 2 0 0 0 2 2h6a2 2 0 0 0 2-2l1-12"></path><path d="M9 7V4h6v3"></path>',
    copy: '<rect x="9" y="9" width="11" height="11" rx="2"></rect><path d="M5 15V5a2 2 0 0 1 2-2h10"></path>',
    refresh: '<path d="M21 12a9 9 0 1 1-3-6.7L21 8"></path><path d="M21 3v5h-5"></path>',
    back: '<path d="M15 18l-6-6 6-6"></path>',
  };
  const svg = (path, size) => '<svg width="' + (size || 18) + '" height="' + (size || 18) + '" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true">' + path + '</svg>';
  const iconButton = (label, path, attrs) => '<button class="st-icon" title="' + label + '" aria-label="' + label + '" ' + attrs + '>' + svg(path) + '</button>';
  const control = (inner, slot) => '<span class="st-ctl">' + inner + (slot || '<span class="st-gap"></span>') + '</span>';
  const row = (title, note, ctl) => '<div class="ol-row"><span class="t"><b>' + title + '</b>' + (note ? '<small class="' + note.cls + '">' + note.text + '</small>' : '') + '</span>' + (ctl || '') + '</div>';
  const toggle = (title, attrs, on) => '<label class="ol-row st-switch"><span class="t"><b>' + title + '</b></span><span class="st-ctl st-end"><input type="checkbox" class="sw-input" aria-label="' + title + '" ' + attrs + (on ? ' checked' : '') + '></span></label>';
  const link = (title, value, sub) => '<button class="ol-row ol-link st-nav" data-action="lan-sub" data-v="' + sub + '"><span class="t"><b>' + title + '</b></span><span class="st-val">' + value + '</span><span class="ol-chev" aria-hidden="true">›</span></button>';
  const card = (body, head, cls) => '<div class="ol-card' + (cls ? ' ' + cls : '') + '">' + (head ? '<div class="st-gh">' + head + '</div>' : '') + body + '</div>';
  const options = (values, current) => values.map(([value, label]) => '<option value="' + escH(value) + '"' + (String(current) === String(value) ? ' selected' : '') + '>' + escH(label) + '</option>').join('');
  window.LANSettings = {
    info: null,
    loading: false,
    sub: null,     // null | 'devices' | 'users': second-level pages of the 區網分享 tab
    assign: {},    // client id -> user id, changed here and not saved yet
    saveTimer: 0, saving: false, edits: 0, // edits counts changes, so a reply never overwrites newer ones
    refresh() {
      if (!nativeHost || onServer || this.loading) return;
      this.loading = true;
      call('lanInfo').then((r) => { this.loading = false; this.info = r; if (S.route.name === 'settings') render(); });
    },
    users() { return (this.info && this.info.users && this.info.users.users) || [{ id: 'default', name: '一般使用者', canDelete: false, libraries: [] }]; },
    scheduleSave(delay) {
      this.edits++;
      clearTimeout(this.saveTimer);
      this.saveTimer = setTimeout(() => { this.saveTimer = 0; this.save(); }, delay);
    },
    // Users and device assignments go to the host together. A user without a name is not sent (the row asks for one);
    // a device whose user was deleted goes back to the default user.
    async save() {
      if (this.saving) { this.saveAgain = true; return; }
      const profiles = this.users().map((u) => ({ id: u.id, name: String(u.name || '').trim(), canDelete: !!u.canDelete, libraries: (u.libraries || []).slice() }));
      if (profiles.some((u) => !u.name)) return;
      const ids = new Set(profiles.map((u) => u.id));
      const st = (this.info && this.info.status) || {};
      const assignments = (st.clients || []).map((c) => {
        const userId = this.assign[c.id] || c.userId || 'default';
        return { clientId: c.id, userId: ids.has(userId) ? userId : 'default' };
      });
      const edits = this.edits;
      this.saving = true;
      let result = null;
      try { result = await call('lanUsersSave', { users: JSON.stringify(profiles), assignments: JSON.stringify(assignments) }); } catch { result = null; }
      this.saving = false;
      if (!result || !result.saved) toast('無法套用使用者設定，請確認媒體庫設定後再試');
      else if (edits === this.edits) { this.assign = {}; this.info = result; if (S.route.name === 'settings') render(); }
      else if (result.status) this.info.status = result.status; // newer edits wait for their own save
      if (this.saveAgain) { this.saveAgain = false; this.save(); }
    },
    // 1.0.0: 區網分享 is not offered yet. The tab shows every option greyed out and the host never opens a LAN port.
    locked: true,
    lockedHtml() {
      const off = (inner) => '<span class="st-ctl">' + inner + '<span class="st-gap"></span></span>';
      const navRow = (title, value) => '<div class="ol-row ol-link st-nav"><span class="t"><b>' + title + '</b></span><span class="st-val">' + value + '</span><span class="ol-chev" aria-hidden="true">›</span></div>';
      return '<div class="ol st-locked">' +
        '<div class="st-locked-body" aria-disabled="true" inert>' +
          card(row('這台電腦的角色', null, off('<select class="input st-sel" disabled aria-label="這台電腦的角色">' + options([['standalone', '單機'], ['server', '伺服器'], ['client', '用戶端']], 'standalone') + '</select>')) +
            row('名稱', null, off('<input class="input st-sel" disabled aria-label="名稱" placeholder="例如：書房 PC">'))) +
          card(row('手機網址', null, off('<span class="st-big st-small">—</span>')) + row('配對 PIN', null, off('<span class="st-big st-small">—</span>'))) +
          card(navRow('已配對裝置', '—') + navRow('使用者與權限', '—')) +
          card(row('串流方式', null, off('<select class="input st-sel" disabled aria-label="串流方式">' + options([['original', '原始檔'], ['superres', '伺服器超分（測試）']], 'original') + '</select>')), '進階') +
        '</div>' +
        '<p class="st-locked-note" role="note">區網分享功能目前尚未開放，以上選項暫時無法使用；播放器不會開啟任何區網連接埠。</p>' +
      '</div>';
    },
    html(draftConfig) {
      if (onServer || isConnect) return '';
      if (this.locked) return this.lockedHtml();
      const lan = draftConfig.lan || (draftConfig.lan = { mode: 'standalone', port: 47880, name: '' });
      const mode = lan.mode || 'standalone';
      const streamMode = lan.streamMode === 'superres' ? 'superres' : 'original';
      if (!this.info) this.refresh();
      const st = (this.info && this.info.status) || {};
      const running = st.running && this.info.mode === 'server';
      const users = this.users();
      const libraries = (this.info && this.info.libraries) || (draftConfig.libraries || []).map((l) => ({ id: l.id, name: l.name }));
      const clients = (running && st.clients) || [];
      if (mode !== 'server' || !running) this.sub = null;
      const subPage = (title, body) => '<div class="ol"><div class="ol-subhead"><button class="ol-back" data-action="lan-sub" data-v="" aria-label="返回區網分享">' + svg(ICON.back, 22) + '</button>' +
        '<div><span class="ol-crumb">區網分享</span><h2>' + title + '</h2></div></div>' + body + '</div>';

      if (this.sub === 'devices') {
        return subPage('已配對裝置', clients.length
          ? card(clients.map((c) => {
            const assigned = this.assign[c.id] || c.userId || 'default';
            return row(escH(c.device || '裝置'), null, control('<select class="input st-sel lan-client-user" data-client-id="' + escH(c.id) + '" aria-label="' + escH(c.device || '裝置') + ' 的使用者">' +
              options(users.map((u) => [u.id, u.name]), users.some((u) => u.id === assigned) ? assigned : 'default') + '</select>',
              iconButton('移除這台裝置', ICON.trash, 'data-action="lan-revoke" data-id="' + escH(c.id) + '"')));
          }).join(''))
          : card(row('尚未配對任何裝置')));
      }
      if (this.sub === 'users') {
        return subPage('使用者與權限', users.map((u) => card(
          row('名稱', String(u.name || '').trim() ? null : { cls: 'st-warn', text: '請輸入名稱，輸入前不會套用。' }, control('<input class="input st-sel" data-user-name data-user-id="' + escH(u.id) + '" aria-label="使用者名稱" value="' + escH(u.name) + '">')) +
          '<div class="st-gh st-gh-mid">可閱覽的媒體庫</div>' +
          (libraries.length ? libraries.map((l) => toggle(escH(l.name || l.id), 'data-user-library="' + escH(l.id) + '" data-user-id="' + escH(u.id) + '"', (u.libraries || []).includes(l.id))).join('')
            : row('目前沒有媒體庫')) +
          '<div class="st-gh st-gh-mid">權限</div>' +
          toggle('允許刪除影片（移至資源回收筒）', 'data-user-delete data-user-id="' + escH(u.id) + '"', u.canDelete) +
          (u.id === 'default' ? '' : row('刪除這位使用者', null, control('<button class="small-btn st-btn danger" data-action="lan-user-remove" data-id="' + escH(u.id) + '">刪除</button>'))),
          escH(u.name) + (u.id === 'default' ? '（預設）' : ''))).join('') +
          '<div class="st-actions"><button class="small-btn" data-action="lan-user-add">新增使用者</button></div>');
      }

      let html = card(row('這台電腦的角色', null, control('<select class="input st-sel" id="lan-role" aria-label="這台電腦的角色">' +
          options([['standalone', '單機'], ['server', '伺服器'], ['client', '用戶端']], mode) + '</select>')) +
        (mode === 'standalone' ? '' : row('名稱', null, control('<input class="input st-sel" id="lan-name" aria-label="名稱" placeholder="例如：書房 PC" value="' + escH(lan.name || '') + '">'))));
      if (mode === 'server') {
        if (running) {
          const url = st.addresses && st.addresses.length ? 'http://' + st.addresses[0] + ':' + st.port : '';
          html += card(
            row('手機網址', null, control('<span class="st-big">' + escH(url || '找不到區網位址') + '</span>', url ? iconButton('複製網址', ICON.copy, 'data-action="lan-copy-url" data-v="' + escH(url) + '"') : '')) +
            (st.addresses && st.addresses.length > 1 ? row('其他位址', null, control('<span class="st-big st-small">' + st.addresses.slice(1).map((a) => escH('http://' + a + ':' + st.port)).join('、') + '</span>')) : '') +
            row('配對 PIN', null, control('<span class="st-big st-pin">' + escH((st.pin || '').replace(/(\d{3})(\d{3})/, '$1 $2')) + '</span>', iconButton('產生新的 PIN', ICON.refresh, 'data-action="lan-newpin"')))) +
            card(link('已配對裝置', clients.length + ' 台', 'devices') + link('使用者與權限', users.length + ' 位使用者', 'users'));
        } else {
          html += card(this.info && this.info.mode === 'server'
            ? row('狀態', { cls: 'st-bad', text: '無法啟動：連接埠 ' + escH(lan.port || 47880) + ' 可能被占用，請看 data\\logs\\lan.log。' })
            : row('狀態', null, control('<span class="st-big st-small">正在啟動…</span>')));
        }
        html += card(row('串流方式', streamMode === 'superres' ? { cls: 'st-warn', text: '測試中：使用伺服器目前的超分設定，沒有聲音；HDR 或不支援的素材會停止並顯示錯誤。' } : null,
          control('<select class="input st-sel" id="lan-stream-mode" aria-label="串流方式">' + options([['original', '原始檔'], ['superres', '伺服器超分（測試）']], streamMode) + '</select>')), '進階');
      }
      return '<div class="ol">' + html + '</div>';
    },
    actions: {
      'lan-sub': (d) => { LANSettings.sub = d.v || null; render(); const c = document.querySelector('.set-content'); if (c) c.scrollTop = 0; },
      'lan-copy-url': (d) => {
        const done = () => toast('已複製網址');
        if (navigator.clipboard && navigator.clipboard.writeText) navigator.clipboard.writeText(d.v).then(done, () => toast('無法複製網址'));
        else toast('無法複製網址');
      },
      'lan-newpin': () => call('lanNewPin').then((r) => { LANSettings.info = r; render(); }),
      'lan-revoke': (d) => call('lanRevoke', { id: d.id }).then((r) => { delete LANSettings.assign[d.id]; LANSettings.info = r; render(); }),
      'lan-user-add': () => {
        const profiles = LANSettings.users();
        profiles.push({ id: 'u-' + Math.random().toString(36).slice(2, 10), name: '新使用者', canDelete: false, libraries: [] });
        LANSettings.info = LANSettings.info || {};
        LANSettings.info.users = { users: profiles };
        LANSettings.scheduleSave(0);
        render();
      },
      'lan-user-remove': (d) => {
        LANSettings.info = LANSettings.info || {};
        LANSettings.info.users = { users: LANSettings.users().filter((u) => u.id !== d.id) };
        LANSettings.scheduleSave(0);
        render();
      },
    },
  };
  const profileOf = (target) => LANSettings.users().find((u) => u.id === target.dataset.userId);
  document.addEventListener('input', (event) => {
    const target = event.target;
    if (target.id === 'lan-name' && typeof draft !== 'undefined' && draft) { draft.lan = draft.lan || { mode: 'standalone', port: 47880 }; draft.lan.name = target.value; }
    else if (target.matches && target.matches('[data-user-name]')) {
      const u = profileOf(target);
      if (!u) return;
      u.name = target.value;
      // Show or clear the 請輸入名稱 note in place: a render would rebuild the row and take the caret away.
      const label = target.closest('.ol-row') && target.closest('.ol-row').querySelector('.t');
      const note = label && label.querySelector('small');
      if (label && !target.value.trim() && !note) label.insertAdjacentHTML('beforeend', '<small class="st-warn">請輸入名稱，輸入前不會套用。</small>');
      else if (note && target.value.trim()) note.remove();
      LANSettings.scheduleSave(700);
    }
  });
  document.addEventListener('change', (event) => {
    const target = event.target;
    if (typeof draft === 'undefined' || !draft || !target.matches) return;
    if (target.id === 'lan-role') {
      draft.lan = Object.assign(draft.lan || {}, { mode: target.value });
      if (!draft.lan.port) draft.lan.port = 47880;
      render();
    } else if (target.id === 'lan-stream-mode') {
      draft.lan = Object.assign(draft.lan || {}, { mode: 'server' });
      draft.lan.streamMode = target.value === 'superres' ? 'superres' : 'original';
      render();
    } else if (target.matches('.lan-client-user')) { LANSettings.assign[target.dataset.clientId] = target.value; LANSettings.scheduleSave(150); }
    else if (target.matches('[data-user-library]')) {
      const u = profileOf(target);
      if (u) u.libraries = target.checked ? [...new Set([...(u.libraries || []), target.dataset.userLibrary])] : (u.libraries || []).filter((id) => id !== target.dataset.userLibrary);
      LANSettings.scheduleSave(150);
    } else if (target.matches('[data-user-delete]')) { const u = profileOf(target); if (u) u.canDelete = target.checked; LANSettings.scheduleSave(150); }
  });

  if (!onServer && !isConnect) { window.LAN = null; return; }

  // ================================================================= common LAN object
  const LAN = window.LAN = {
    mode: onServer ? 'remote' : 'connect',
    native: !!nativeHost,
    phone: false,
    serverName: '',
    streamMode: 'original',
    token: '',
    session: 0,
    ready: null,
  };
  const applyServerCapabilities = (value) => {
    LAN.streamMode = value.streamMode === 'superres' ? 'superres' : 'original';
    LAN.browserStreams = !!value.browserStreams;
  };
  const STREAM_PREFERENCES_KEY = 'vp_lan_stream_preferences';
  const streamPreferences = () => {
    const saved = store.get(STREAM_PREFERENCES_KEY, {});
    return {
      upscale: typeof saved.upscale === 'boolean' ? saved.upscale : LAN.streamMode === 'superres',
      interpolation: typeof saved.interpolation === 'boolean' ? saved.interpolation : false,
    };
  };
  const saveStreamPreferences = (value) => store.set(STREAM_PREFERENCES_KEY, value);
  const phoneQuery = window.matchMedia('(max-width: 820px), (pointer: coarse) and (max-width: 1100px)');
  // Chrome's "desktop site" mode widens the viewport to ~980px; fall back to touch + physical screen width.
  const touchPhone = navigator.maxTouchPoints > 0 && Math.min(screen.width, screen.height) <= 600;
  LAN.phone = onServer && !nativeHost && (phoneQuery.matches || touchPhone);
  document.documentElement.classList.add(onServer ? 'lan' : 'lan-connect');
  if (onServer && !nativeHost) document.documentElement.classList.add('lan-browser');
  // Only registers on a secure origin (https / localhost / Chrome's insecure-origin flag); no-op otherwise.
  if (onServer && !nativeHost && 'serviceWorker' in navigator && window.isSecureContext) {
    navigator.serviceWorker.register('sw.js', { scope: './' }).catch(() => {});
  }
  if (LAN.phone) document.documentElement.classList.add('lan-phone');

  // ================================================================= connect page (Windows client)
  if (isConnect) {
    LAN.ready = new Promise((resolve) => {
      const show = () => { renderConnect(); resolve(false); };
      if (document.readyState === 'loading') document.addEventListener('DOMContentLoaded', show); else show();
    });
    const C = { servers: null, busy: false, offline: params.has('offline'), info: null };
    LAN.renderConnect = renderConnect;
    function renderConnect() {
      document.getElementById('shell').hidden = true;
      const splashNode = document.getElementById('splash');
      if (splashNode) splashNode.remove();
      let box = document.getElementById('lan-connect');
      if (!box) { box = document.createElement('div'); box.id = 'lan-connect'; box.className = 'lan-connect'; document.body.appendChild(box); }
      const list = C.servers == null ? '<div class="note" style="margin:14px 0">正在搜尋區網中的伺服器…</div>'
        : C.servers.length ? C.servers.map((s) => '<div class="lan-srv"><span class="dot"></span><div style="flex:1;min-width:0"><b>' + escH(s.name) + '</b><div class="note">' + escH(s.address.replace('http://', '')) + '</div></div>' +
          '<button class="btn pri" data-action="lan-connect" data-server="' + escH(s.address) + '">連線</button></div>').join('')
        : '<div class="note" style="margin:14px 0">沒有找到伺服器。請確認伺服器那台已在「設定 › 區網分享」選擇「伺服器」，或直接輸入位址。</div>';
      box.innerHTML = '<div class="lan-panel glass">' +
        '<h1 class="lan-h">選擇要連線的電腦</h1>' +
        '<p class="note">這台電腦只負責播放；放大與補幀不會在這裡執行。</p>' +
        (C.offline ? '<p class="lan-warn">無法連線到上次的伺服器，請確認它已開機並啟用區網分享。</p>' : '') +
        list +
        '<form class="lan-manual" id="lan-manual"><input class="input" id="lan-addr" placeholder="或輸入位址：192.168.1.10" autocomplete="off"><button class="btn sec" type="submit">連線</button></form>' +
        '<div class="row" style="justify-content:space-between;margin-top:10px"><button class="small-btn" data-action="lan-rescan">⟳ 重新搜尋</button><button class="small-btn" data-action="lan-standalone">改回單機模式 ›</button></div>' +
        '</div>';
      const form = document.getElementById('lan-manual');
      form.addEventListener('submit', (event) => { event.preventDefault(); const v = document.getElementById('lan-addr').value.trim(); if (v) send('lanConnect', { server: v }); });
      if (C.servers == null && !C.busy) discover();
    }
    function discover() {
      C.busy = true;
      call('lanDiscover').then((r) => { C.busy = false; C.servers = r.servers || []; renderConnect(); });
    }
    LAN.actions = {
      'lan-connect': (d) => send('lanConnect', { server: d.server }),
      'lan-rescan': () => { C.servers = null; renderConnect(); },
      'lan-standalone': async () => {
        const reply = await call('getConfig');
        let config;
        try { config = JSON.parse(reply.text); } catch { config = {}; }
        config.lan = Object.assign(config.lan || {}, { mode: 'standalone' });
        const saved = await call('setConfig', { text: JSON.stringify(config, null, 2), restartAudio: false });
        if (saved.ok) { send('lanApply'); setTimeout(() => location.replace('https://app.local/index.html'), 300); }
      },
    };
    return;
  }

  // ================================================================= remote page: transport
  LAN.token = store.get('vp_lan_token', '') || ((document.cookie.match(/vp_token=([0-9a-f]{32})/) || [])[1] || '');
  const headers = () => (LAN.token ? { 'X-VP-Token': LAN.token } : {});
  const withToken = (url) => url + (url.includes('?') ? '&' : '?') + 't=' + encodeURIComponent(LAN.token);
  LAN.fileUrl = (path) => withToken(location.origin + '/stream/file?p=' + encodeURIComponent(path));
  LAN.subUrl = (path) => withToken(location.origin + '/stream/sub?p=' + encodeURIComponent(path));
  // Host-local URLs (https://cache.local/... from the server's data) as server paths.
  LAN.url = (u) => String(u || '').replace(/^https:\/\/(media|cache|thumbs|fonts)\.local\//, (m, h) => '/' + h + '/');
  // Embedded fonts: a Windows client's own core extracted them into its local cache.
  LAN.fontUrl = (f) => (nativeHost ? 'https://cache.local/' : '/cache/') + f.split('/').map(encodeURIComponent).join('/');
  LAN.systemFontBase = nativeHost ? 'https://fonts.local/' : '/fonts/';

  function replyTo(args, type, fields) { if (args && args.reqId) deliver(Object.assign({ type, reqId: args.reqId }, fields || {})); }
  async function getJson(url) {
    const r = await fetch(url, { headers: headers(), credentials: 'same-origin', cache: 'no-store' });
    if (!r.ok) throw new Error('HTTP ' + r.status);
    return r.json();
  }

  // ---- startup: pairing, then the event stream
  LAN.ready = (async () => {
    let hello;
    try { hello = await getJson('/api/hello'); } catch { hello = { name: location.host, authorized: false, unreachable: true }; }
    LAN.serverName = hello.name || location.host;
    applyServerCapabilities(hello);
    if (!hello.authorized) await pairScreen(hello);
    if (!hello.authorized) {
      try {
        hello = await getJson('/api/hello');
        applyServerCapabilities(hello);
      } catch { /* pairing can still start the event stream */ }
    }
    LAN.serverId = hello.id || location.host;
    LAN.userId = hello.userId || 'default';
    LAN.canDelete = !!hello.canDelete;
    LAN.libraryIds = JSON.stringify((hello.libraries || []).slice().sort());
    await openEvents();
    return true;
  })();

  function pairScreen(hello) {
    return new Promise((resolve) => {
      const mount = () => {
        const box = document.createElement('div');
        box.id = 'lan-pair';
        box.className = 'lan-pair';
        box.innerHTML = '<div class="lan-pair-in">' +
          '<div class="lan-logo"></div>' +
          '<h1>連線到「' + escH(hello.name || location.host) + '」</h1>' +
          '<p class="note">在電腦的「設定 › 區網分享」可以看到 6 位數 PIN</p>' +
          '<label class="lan-pin-boxes"><input id="lan-pin" inputmode="numeric" pattern="[0-9]*" maxlength="6" autocomplete="one-time-code" aria-label="PIN">' +
          '<span></span><span></span><span></span><span></span><span></span><span></span></label>' +
          '<button class="btn pri" id="lan-pair-go">連線</button>' +
          '<p class="lan-warn" id="lan-pair-err" hidden></p>' +
          '<p class="note">管理員會為這台裝置指派可閱覽的媒體庫與刪除權限。</p></div>';
        document.body.appendChild(box);
        const input = box.querySelector('#lan-pin');
        const cells = [...box.querySelectorAll('.lan-pin-boxes span')];
        const paint = () => cells.forEach((c, i) => { c.textContent = input.value[i] || ''; c.classList.toggle('cur', i === input.value.length); });
        input.addEventListener('input', () => { input.value = input.value.replace(/\D/g, '').slice(0, 6); paint(); if (input.value.length === 6) submit(); });
        const err = box.querySelector('#lan-pair-err');
        const device = /Android/i.test(navigator.userAgent) ? 'Android ' + ((navigator.userAgent.match(/;\s*([^;)]+)\s+Build/) || [])[1] || '裝置')
          : nativeHost ? 'Windows 用戶端' : (navigator.userAgentData && navigator.userAgentData.platform) || '瀏覽器';
        async function submit() {
          err.hidden = true;
          try {
            const r = await fetch('/api/pair', { method: 'POST', body: JSON.stringify({ pin: input.value, device }), credentials: 'same-origin' });
            const body = await r.json();
            if (body.ok) { LAN.token = body.token; store.set('vp_lan_token', body.token); box.remove(); resolve(); return; }
            err.textContent = body.error === 'locked' ? '錯誤太多次，請等 1 分鐘再試' : 'PIN 不正確';
          } catch { err.textContent = '無法連線到伺服器'; }
          err.hidden = false;
          input.value = ''; paint(); input.focus();
        }
        box.querySelector('#lan-pair-go').addEventListener('click', submit);
        paint();
        setTimeout(() => input.focus(), 50);
        const splashNode = document.getElementById('splash');
        if (splashNode) splashNode.remove();
      };
      if (document.readyState === 'loading') document.addEventListener('DOMContentLoaded', mount); else mount();
    });
  }

  function openEvents() {
    return new Promise((resolve) => {
      let es = null;
      const connect = () => {
        es = new EventSource('/api/events' + (LAN.token ? '?t=' + encodeURIComponent(LAN.token) : ''));
        es.onmessage = (event) => {
          let message;
          try { message = JSON.parse(event.data); } catch { return; }
          if (message.type === 'session') {
            const previous = LAN.session;
            LAN.session = message.id;
            if (previous && previous !== LAN.session) getJson('/api/hello').then((hello) => {
              const libraries = JSON.stringify((hello.libraries || []).slice().sort());
              if (hello.userId !== LAN.userId || !!hello.canDelete !== LAN.canDelete || libraries !== LAN.libraryIds) location.reload();
            }).catch(() => {});
            resolve(); return;
          }
          if (message.type === 'denied') { if (message.reqId) deliver(message); return; }
          deliver(message);
        };
        es.onerror = () => { /* EventSource reconnects by itself; a new session id arrives */ };
      };
      connect();
    });
  }

  function rpc(cmd, args) {
    const body = JSON.stringify(Object.assign({ cmd }, args || {}));
    fetch('/api/rpc?s=' + LAN.session, { method: 'POST', body, headers: headers(), credentials: 'same-origin' })
      .then((r) => { if (!r.ok && args && args.reqId) replyTo(args, 'denied', { status: r.status }); })
      .catch(() => { if (args && args.reqId) replyTo(args, 'denied', { status: 0 }); });
  }

  // Preferences that belong to this device (layout choices, motion). Everything else - custom
  // posters, metadata choices - comes from the server and is never written back.
  const LOCAL_PREFS = ['homeLayout', 'epLayout', 'posterLeft', 'split', 'segInterval', 'remaining', 'motion', 'subtitles', 'lanTab', 'lanFilter'];
  const RPC = new Set(['scan', 'thumbnails', 'thumbnailsAt', 'cancelThumbnails', 'artwork', 'getConfig']);
  // setVolume / setMute: the player-bar level (the native host applies them to its own player like the other playback commands).
  const PLAY = new Set(['open', 'play', 'pause', 'toggle', 'seek', 'seekRelative', 'selectSubtitle', 'loadSubtitle', 'stop', 'fullscreen', 'setVolume', 'setMute']);

  LAN.send = function (cmd, args) {
    args = args || {};
    if (RPC.has(cmd)) { rpc(cmd, args); return true; }
    if (cmd === 'loadData') { loadData(args); return true; }
    if (cmd === 'saveData') {
      if (args.key === 'progress') {
        let progress = {};
        try { progress = JSON.parse(args.text || '{}'); } catch { /* ignore invalid local state */ }
        store.set(progressKey(), progress);
        // 1B-b: the server answers 400 when it would not store this user's progress (its lan_progress file is unreadable,
        // empty or corrupt, or the write failed); the page says so once. The progress stays on this device either way.
        fetch('/api/data?key=progress', { method: 'POST', body: JSON.stringify(progress), headers: Object.assign({ 'Content-Type': 'application/json' }, headers()), credentials: 'same-origin', cache: 'no-store' })
          .then((r) => replyTo(args, 'dataSaved', r.ok ? { ok: true } : { ok: false, reason: r.status === 400 ? 'server-refused' : 'http-' + r.status }))
          .catch(() => replyTo(args, 'dataSaved', { ok: false, reason: 'offline' }));
        return true;
      }
      else if (args.key === 'prefs') {
        const all = JSON.parse(args.text || '{}');
        store.set('vp_lan_prefs', Object.fromEntries(LOCAL_PREFS.filter((k) => k in all).map((k) => [k, all[k]])));
      }
      replyTo(args, 'dataSaved', { ok: true });
      return true;
    }
    if (cmd === 'http' || cmd === 'anilist') { replyTo(args, 'http', { status: 0, retryAfter: 0, text: '' }); return true; }
    if (PLAY.has(cmd)) {
      if (nativeHost) {
        if (cmd === 'open') nativeHost.postMessage(Object.assign({}, args, { cmd, url: LAN.fileUrl(args.path), path: undefined }));
        else if (cmd === 'loadSubtitle') nativeHost.postMessage({ cmd, url: LAN.subUrl(args.path) });
        else nativeHost.postMessage(Object.assign({ cmd }, args));
      } else {
        Player.handle(cmd, args);
      }
      return true;
    }
    if (cmd === 'lanDisconnect' && nativeHost) { nativeHost.postMessage({ cmd }); return true; }
    // Anything that changes the server (setConfig, saveImage, pickers, ASIO, ...) is refused.
    if (cmd === 'setConfig') replyTo(args, 'configSaved', { ok: false });
    else replyTo(args, 'denied', {});
    return true;
  };

  async function loadData(args) {
    const key = args.key;
    let text = '';
    try {
      if (key === 'progress') {
        const r = await getJson('/api/data?key=progress');
        const remote = parseObject(r.text);
        const local = progressForUser();
        const merged = mergeProgress(local, remote);
        text = JSON.stringify(merged);
        store.set(progressKey(), merged);
        if (JSON.stringify(merged) !== JSON.stringify(remote)) {
          fetch('/api/data?key=progress', { method: 'POST', body: JSON.stringify(merged), headers: Object.assign({ 'Content-Type': 'application/json' }, headers()), credentials: 'same-origin', cache: 'no-store' }).catch(() => {});
        }
      } else {
        const r = await getJson('/api/data?key=' + encodeURIComponent(key));
        text = r.text || '';
        if (key === 'prefs') {
          let prefs = {};
          try { prefs = text ? JSON.parse(text) : {}; } catch { prefs = {}; }
          for (const k of LOCAL_PREFS) delete prefs[k];
          text = JSON.stringify(Object.assign(prefs, store.get('vp_lan_prefs', {})));
        }
      }
    } catch (error) { console.warn('lan loadData', key, error); }
    replyTo(args, 'data', { key, text });
  }

  function progressKey() { return 'vp_lan_progress_' + String(LAN.serverId || 'server') + '_' + String(LAN.userId || 'default'); }
  function parseObject(text) { try { const value = text ? JSON.parse(text) : {}; return value && typeof value === 'object' && !Array.isArray(value) ? value : {}; } catch { return {}; } }
  function progressForUser() {
    let local = store.get(progressKey(), null);
    const migrationKey = 'vp_lan_progress_imported_' + String(LAN.serverId || 'server');
    if (LAN.userId === 'default' && local == null && !store.get(migrationKey, false)) {
      local = store.get('vp_lan_progress', {});
      store.set(migrationKey, true);
    }
    return parseObject(JSON.stringify(local || {}));
  }
  function mergeProgress(local, remote) {
    const merged = Object.assign({}, remote || {});
    for (const [path, entry] of Object.entries(local || {})) {
      if (!merged[path] || (+entry?.updated || 0) > (+merged[path]?.updated || 0)) merged[path] = entry;
    }
    return merged;
  }

  // Actions that would change the server's data are not available on a client.
  LAN.blocked = new Set(['settings', 'open-file', 'add-folder', 'remove-folder', 'toggle-frame-interpolation', 'toggle-upscale', 'refresh-asio', 'restart-audio',
    'settings-save', 'settings-cancel', 'devtools', 'al-open', 'al-review-pick', 'al-review-keep', 'al-cand-reject', 'al-dataset-accept', 'al-dataset-reject', 'al-dataset-ignore', 'al-close', 'al-pick', 'al-ignore', 'al-pref', 'al-lib', 'al-retry', 'poster-open', 'backdrop-open',
    'poster-scope', 'poster-filter', 'poster-pick', 'poster-reset', 'season-src-open', 'season-src-set', 'season-src-panel', 'season-src-clear', 'season-src-reset',
    'season-hit', 'season-tmdb-apply', 'season-bgm-apply', 'tmdb-key', 'bgm-token', 'subtitle-pick', 'person',
    // Phase 1B-a β: season locks are set on the player host only (the lock status is still shown, read-only)
    'season-lock-ask', 'season-lock-confirm', 'season-lock-cancel', 'season-unlock-ask', 'season-unlock-confirm', 'season-lock-hint-close',
    'season-lock-reattach', 'season-lock-reattach-skip', 'al-ignore-confirm', 'al-ignore-cancel',
    // Phase 1B-a δ: 只用 TMDB is set on the player host only
    'al-tmdb-only', 'al-tmdb-only-confirm',
    // Phase 1B-a ε: the confirmed TMDB pick that replaces an incompatible AniList entry
    'al-pick-replace-confirm',
    // 刷新中繼資料 (per item, per season, all, cancel): the player host only
    'meta-refresh-item', 'meta-refresh-season', 'meta-refresh-all', 'meta-refresh-cancel',
    // Phase 3B: episode overrides (單集對應, 這一集的資料…, orphan re-attach) are set on the player host only; the applied
    // titles, synopses, stills and labels are still shown (same file, same readers)
    'ep-ov-open', 'ep-ov-save', 'ep-ov-reset', 'ep-ov-map', 'ep-ov-mark', 'ep-ov-reattach', 'ep-ov-delete', 'ep-ov-panel', 'ep-ov-still', 'ep-ov-season', 'ep-ov-close']);

  // ================================================================= browser player (<video> + /stream/remux)
  const Player = LAN.player = {
    video: null, path: '', probe: null, offset: 0, audio: -1, timer: 0, token: 0, wantPaused: false, lastState: '',
    metadataHandler: null,
    serverSuperResolution: false, serverFrameInterpolation: false, serverProcessed: false,
    ensureVideo() {
      if (this.video) return this.video;
      const v = document.createElement('video');
      v.id = 'lan-video';
      v.playsInline = true;
      v.setAttribute('playsinline', '');
      v.preload = 'auto';
      const host = document.getElementById('player');
      host.insertBefore(v, host.firstChild);
      v.addEventListener('playing', () => this.state('playing'));
      v.addEventListener('pause', () => { if (!v.ended && this.path) this.state('paused'); });
      v.addEventListener('waiting', () => this.state('seeking'));
      v.addEventListener('ended', () => { this.emit('ended', {}); this.state('ended'); });
      v.addEventListener('error', () => {
        if (!this.path || !v.getAttribute('src')) return;
        this.emit('error', { message: '瀏覽器無法播放這個影片（' + this.describe() + '）。這台裝置可能不支援此影片編碼。', code: 'browser' });
      });
      v.addEventListener('loadedmetadata', () => { if (typeof relayoutSubtitles === 'function') relayoutSubtitles(); LAN.layoutPlayer(); });
      this.video = v;
      return v;
    },
    describe() {
      const p = this.probe;
      return p && p.video ? String(p.video.codec).toUpperCase() + ' ' + p.video.width + '×' + p.video.height + (p.video.pixFmt && /10/.test(p.video.pixFmt) ? ' 10-bit' : '') : '未知格式';
    },
    emit(event, data) { deliver({ type: 'player', event, data }); },
    state(s) { if (s === this.lastState) return; this.lastState = s; this.emit('state', { state: s }); },
    pos() { return this.video ? this.offset + this.video.currentTime * 1000 : 0; },
    dur() { return this.probe ? this.probe.durationMs : 0; },
    tick() {
      clearInterval(this.timer);
      this.timer = setInterval(() => {
        if (!this.path || !this.video) return;
        this.emit('position', { position_ms: Math.round(this.pos()), duration_ms: this.dur(), clock: 'browser', fps: 0, dropped: 0, av_offset_ms: 0 });
      }, 250);
    },
    async handle(cmd, a) {
      const v = this.ensureVideo();
      if (cmd === 'open') return this.open(a);
      if (cmd === 'play') return v.play().catch(() => {});
      if (cmd === 'pause') return v.pause();
      if (cmd === 'toggle') return v.paused ? v.play().catch(() => {}) : v.pause();
      if (cmd === 'seek') return this.seek(a.ms);
      if (cmd === 'seekRelative') return this.seek(this.pos() + a.ms);
      if (cmd === 'stop') return this.stop();
      if (cmd === 'selectSubtitle') return this.subtitle(a.index);
      if (cmd === 'loadSubtitle') return this.sidecar(a.path);
      if (cmd === 'fullscreen') return this.fullscreen(a.on);
      if (cmd === 'setVolume') { v.volume = Math.min(1, Math.max(0, (Number(a.value) || 0) / 100)); return; }
      if (cmd === 'setMute') { v.muted = !!a.value; return; }
    },
    async open(a) {
      const token = ++this.token;
      this.path = a.path;
      if (!nativeHost) {
        try { applyServerCapabilities(await getJson('/api/hello')); } catch { /* keep the last known server mode */ }
      }
      if (token !== this.token) return;
      const preferences = streamPreferences();
      this.serverSuperResolution = !nativeHost && preferences.upscale;
      this.serverFrameInterpolation = !nativeHost && preferences.interpolation;
      this.serverProcessed = this.serverSuperResolution || this.serverFrameInterpolation;
      this.audio = -1;
      this.wantPaused = !!a.paused;
      this.lastState = '';
      this.state('opening');
      let probe;
      try { probe = await getJson('/api/probe?p=' + encodeURIComponent(a.path)); } catch (e) { probe = null; }
      if (token !== this.token) return;
      if (!probe || !probe.video) { this.emit('error', { message: '伺服器無法讀取這個檔案', code: 'probe' }); return; }
      this.probe = probe;
      if (!probe.video.browser && !this.serverProcessed) { this.emit('error', { message: '這個影片的編碼（' + this.describe() + '）無法在瀏覽器播放。', code: 'codec' }); return; }
      const best = (probe.audio || []).find((t) => t.best) || (probe.audio || [])[0];
      this.audio = best ? best.index : -1;
      this.emit('opened', {
        duration_ms: probe.durationMs, clock: 'browser', width: probe.video.width, height: probe.video.height, video_codec: probe.video.codec,
        subtitles: (probe.subtitles || []).map((t) => ({ index: t.index, language: t.lang, title: t.title, codec: t.codec, forced: t.forced, default: t.default, text: t.text })),
        audio_tracks: this.serverProcessed ? [] : probe.audio || [], chapters: [], fonts: probe.fonts || [],
      });
      await this.load(a.startMs || 0);
      this.tick();
    },
    async load(ms) {
      const token = ++this.token;
      const v = this.ensureVideo();
      if (this.metadataHandler) {
        v.removeEventListener('loadedmetadata', this.metadataHandler);
        this.metadataHandler = null;
      }
      let start = ms;
      if (!this.serverProcessed) {
        try { start = (await getJson('/api/seekpoint?p=' + encodeURIComponent(this.path) + '&ms=' + Math.round(ms))).startMs; } catch { start = ms; }
      }
      if (token !== this.token) return;
      this.offset = start;
      const go = () => {
        if (this.metadataHandler === go) this.metadataHandler = null;
        if (token !== this.token || v !== this.video) return;
        // The stream starts at the key frame before `ms`: skip forward to the exact time.
        const skip = (ms - start) / 1000;
        if (skip > 0.3) { try { v.currentTime = skip; } catch { /* not seekable yet */ } }
        if (!this.wantPaused) v.play().catch(() => this.state('paused')); else this.state('paused');
      };
      this.metadataHandler = go;
      v.addEventListener('loadedmetadata', go, { once: true });
      v.src = withToken('/stream/remux?p=' + encodeURIComponent(this.path) + '&start=' + Math.round(ms) + '&audio=' + this.audio +
        (this.serverSuperResolution ? '&sr=1' : '') + (this.serverFrameInterpolation ? '&frc=1' : ''));
      // Reset the old demuxer/decoder before the new fragmented MP4 stream starts.
      v.load();
    },
    seek(ms) {
      const v = this.video;
      if (!v || !this.path) return;
      ms = Math.max(0, Math.min(ms, this.dur() || ms));
      this.wantPaused = v.paused;
      // Inside what is already buffered: move there; otherwise ask the server for a new stream.
      const local = (ms - this.offset) / 1000;
      for (let i = 0; i < v.buffered.length; i++) {
        if (local >= v.buffered.start(i) && local <= v.buffered.end(i) - 0.5) { v.currentTime = local; return; }
      }
      this.state('seeking');
      this.load(ms);
    },
    stop() {
      ++this.token;
      clearInterval(this.timer);
      this.path = '';
      if (this.video && this.metadataHandler) this.video.removeEventListener('loadedmetadata', this.metadataHandler);
      this.metadataHandler = null;
      this.probe = null;
      this.serverSuperResolution = false;
      this.serverFrameInterpolation = false;
      this.serverProcessed = false;
      if (this.video) { this.video.pause(); this.video.removeAttribute('src'); this.video.load(); }
      if (document.fullscreenElement) document.exitFullscreen().catch(() => {});
      if (this.shellFull) this.fullscreen(false); // leaving playback also leaves the forced landscape
    },
    setAudio(index) {
      if (index === this.audio) return;
      this.audio = index;
      const at = this.pos();
      this.wantPaused = this.video.paused;
      this.state('seeking');
      this.load(at);
    },
    async subtitle(index) {
      if (index < 0) { this.emit('subtitle', { action: 'track', source: 'off', index: -1 }); return; }
      const track = ((this.probe && this.probe.subtitles) || []).find((t) => t.index === index);
      if (track && !track.text) { toast('圖形字幕（' + (CODEC_NAME[track.codec] || track.codec) + '）目前只能在電腦上顯示'); return; }
      try {
        const r = await fetch('/api/subtitle?p=' + encodeURIComponent(this.path) + '&i=' + index, { headers: headers(), credentials: 'same-origin' });
        if (!r.ok) throw new Error(r.status);
        const script = await r.text();
        this.emit('subtitle', { action: 'track', source: 'embedded', index });
        this.emit('subtitle', { action: 'ass_track', header: script, events: [], complete: true });
      } catch { toast('字幕讀取失敗'); }
    },
    async sidecar(path) {
      const ext = (path.match(/\.([^.\\/]+)$/) || [])[1] || '';
      if (/^sup$/i.test(ext)) { toast('圖形字幕（.sup）目前只能在電腦上顯示'); return; }
      try {
        const r = await fetch('/stream/sub?p=' + encodeURIComponent(path), { headers: headers(), credentials: 'same-origin' });
        if (!r.ok) throw new Error(r.status);
        const buffer = await r.arrayBuffer();
        let text;
        try { text = new TextDecoder('utf-8', { fatal: true }).decode(buffer); }
        catch { try { text = new TextDecoder('big5').decode(buffer); } catch { text = new TextDecoder('gbk').decode(buffer); } }
        text = text.replace(/^\uFEFF/, '');
        const script = /^(ass|ssa)$/i.test(ext) ? text : toAss(text);
        this.emit('subtitle', { action: 'track', source: 'external', path, events: (script.match(/^Dialogue:/gm) || []).length });
        this.emit('subtitle', { action: 'ass_track', header: script, events: [], complete: true });
      } catch { toast('字幕讀取失敗'); }
    },
    fullscreen(on) {
      if (window.VPShell && window.VPShell.setLandscape) { // Android shell: rotate natively (the WebView cannot lock orientation itself)
        const next = on == null ? !this.shellFull : !!on;
        this.shellFull = next;
        try { window.VPShell.setLandscape(next); } catch { /* bridge missing */ }
        deliver({ type: 'fullscreen', on: next });
        return;
      }
      const want = on == null ? !document.fullscreenElement : on;
      if (want && !document.fullscreenElement) {
        document.documentElement.requestFullscreen().then(() => { try { screen.orientation.lock('landscape').catch(() => {}); } catch { /* unsupported */ } }).catch(() => {});
      } else if (!want && document.fullscreenElement) {
        document.exitFullscreen().catch(() => {});
      }
    },
  };
  document.addEventListener('fullscreenchange', () => deliver({ type: 'fullscreen', on: !!document.fullscreenElement }));

  // SRT / WebVTT -> ASS for the libass renderer.
  function toAss(text) {
    const t = (s) => {
      const m = s.trim().match(/(?:(\d+):)?(\d+):(\d+)[.,](\d+)/);
      if (!m) return null;
      const ms = (+(m[1] || 0) * 3600 + +m[2] * 60 + +m[3]) * 1000 + Math.round(+('0.' + m[4]) * 1000);
      const cs = Math.round(ms / 10);
      return Math.floor(cs / 360000) + ':' + String(Math.floor(cs / 6000) % 60).padStart(2, '0') + ':' + String(Math.floor(cs / 100) % 60).padStart(2, '0') + '.' + String(cs % 100).padStart(2, '0');
    };
    const lines = [];
    for (const block of text.replace(/\r/g, '').split(/\n{2,}/)) {
      const rows = block.split('\n');
      const at = rows.findIndex((r) => r.includes('-->'));
      if (at < 0) continue;
      const [a, b] = rows[at].split('-->');
      const start = t(a), end = t(b);
      if (!start || !end) continue;
      const body = rows.slice(at + 1).join('\\N').replace(/<\/?(b|i|u)>/gi, (m, tag) => (m[1] === '/' ? '{\\' + tag + '0}' : '{\\' + tag + '1}')).replace(/<[^>]+>/g, '');
      if (body.trim()) lines.push('Dialogue: 0,' + start + ',' + end + ',Default,,0,0,0,,' + body);
    }
    return '[Script Info]\nScriptType: v4.00+\nPlayResX: 1920\nPlayResY: 1080\nWrapStyle: 0\n\n[V4+ Styles]\n' +
      'Format: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, OutlineColour, BackColour, Bold, Italic, Underline, StrikeOut, ScaleX, ScaleY, Spacing, Angle, BorderStyle, Outline, Shadow, Alignment, MarginL, MarginR, MarginV, Encoding\n' +
      'Style: Default,Microsoft JhengHei,62,&H00FFFFFF,&H000000FF,&H00000000,&H64000000,-1,0,0,0,100,100,0,0,1,3,1,2,60,60,50,1\n\n' +
      '[Events]\nFormat: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text\n' + lines.join('\n') + '\n';
  }

  // ================================================================= player geometry (browser)
  // Portrait phone: video on top (16:9 area), controls on the video, episodes / tracks below.
  LAN.portrait = () => LAN.phone && window.innerHeight > window.innerWidth;
  LAN.layoutPlayer = function () {
    const player = document.getElementById('player');
    if (!player || nativeHost) return;
    player.classList.toggle('lan-portrait', LAN.portrait());
    const p = Player.probe;
    const ratio = p && p.video && p.video.width ? p.video.height / p.video.width : 9 / 16;
    document.documentElement.style.setProperty('--lan-vh', Math.round(Math.min(window.innerWidth * ratio, window.innerHeight * 0.45)) + 'px');
    if (LAN.portrait() && P.open) renderUnder();
  };
  // Touch gestures on the picture (phone browser): double-tap left / right = -10 s / +10 s, double-tap middle = fullscreen,
  // press and hold = 2x speed while held.
  function initGestures() {
    const center = document.getElementById('p-center');
    if (!center || center._lanGestures || nativeHost || !LAN.phone) return;
    center._lanGestures = true;
    const playerBox = document.getElementById('player');
    // The <video> sits under #p-center (pointer-events: none), so the picture is whatever is not a control.
    const isPicture = (el) => !el.closest('.p-top, .p-bottom, .p-menu, .p-info, .p-help, .p-upnext, .p-message, #lan-under, button, a, input');
    // Single tap on the picture = show the controls, or hide them at once when they were already showing (play / pause stays on its button).
    let tapTimer = 0, wasHidden = false, suppressUntil = 0;
    playerBox.addEventListener('pointerdown', () => { wasHidden = playerBox.classList.contains('idle'); }, true);
    playerBox.addEventListener('click', (event) => {
      if (!isPicture(event.target)) return;
      event.stopImmediatePropagation(); // not the desktop "click toggles pause"
      clearTimeout(tapTimer);
      if (wasHidden || Date.now() < suppressUntil) return; // the touch already brought the controls back / it was a press-and-hold
      tapTimer = setTimeout(() => { // wait out a possible double-tap (seek / fullscreen)
        playerBox.classList.add('idle');
        if (typeof relayoutSubtitles === 'function') relayoutSubtitles();
      }, 230);
    }, true);
    playerBox.addEventListener('dblclick', (event) => {
      if (!isPicture(event.target)) return;
      event.stopImmediatePropagation();
      clearTimeout(tapTimer);
      clearTimeout(P.clickTimer);
      const box = (Player.video || center).getBoundingClientRect();
      const x = (event.clientX - box.left) / Math.max(1, box.width);
      if (x < 0.35) seekBy(-10000); else if (x > 0.65) seekBy(10000); else send('fullscreen');
    }, true);
    let timer = 0, held = false;
    const release = (event) => {
      clearTimeout(timer);
      if (held) {
        held = false;
        suppressUntil = Date.now() + 500;
        if (Player.video) Player.video.playbackRate = 1;
        if (event && event.cancelable) event.preventDefault();
        clearTimeout(P.clickTimer);
        flash('1× 倍速');
      }
    };
    playerBox.addEventListener('touchstart', (event) => {
      if (!isPicture(event.target) || event.touches.length !== 1) return;
      timer = setTimeout(() => {
        if (!Player.video || P.state !== 'playing') return;
        held = true;
        Player.video.playbackRate = 2;
        flash('2× 倍速');
      }, 450);
    }, { passive: true });
    playerBox.addEventListener('touchmove', () => clearTimeout(timer), { passive: true });
    playerBox.addEventListener('touchend', release, { passive: false });
    playerBox.addEventListener('touchcancel', release, { passive: true });
  }
  if (document.readyState === 'loading') document.addEventListener('DOMContentLoaded', initGestures); else initGestures();
  window.addEventListener('resize', () => { if (P.open) { LAN.layoutPlayer(); if (typeof relayoutSubtitles === 'function') relayoutSubtitles(); } });
  // The picture rectangle inside <video> (object-fit: contain) for subtitles.
  LAN.videoRect = function () {
    const v = Player.video;
    if (nativeHost || !v) return null;
    const box = v.getBoundingClientRect();
    const vw = v.videoWidth || (P.info && P.info.width) || 16, vh = v.videoHeight || (P.info && P.info.height) || 9;
    const scale = Math.min(box.width / vw, box.height / vh);
    const w = vw * scale, h = vh * scale;
    return { left: box.left + (box.width - w) / 2, top: box.top + (box.height - h) / 2, width: w, height: h };
  };

  // Below the video in portrait: title, stream info, 集數 / 字幕 / 音軌.
  const U = { tab: 'eps' };
  function renderUnder() {
    let box = document.getElementById('lan-under');
    const player = document.getElementById('player');
    if (!box) { box = document.createElement('div'); box.id = 'lan-under'; box.className = 'lan-under'; player.appendChild(box); }
    const entry = P.entry;
    const isSeries = entry && entry.lib.kind === 'series';
    const p = Player.probe;
    const audioTrack = p && (p.audio || []).find((t) => t.index === Player.audio);
    const info = p && p.video ? [String(p.video.codec).toUpperCase() + ' ' + p.video.height + 'p', audioTrack ? (audioTrack.copy ? String(audioTrack.codec).toUpperCase() : '音訊轉 AAC') : ''].filter(Boolean).join(' · ') : '';
    const tabs = [['eps', isSeries ? '集數' : '檔案'], ['subs', '字幕'], ['audio', '音軌']];
    let list = '';
    if (U.tab === 'eps') {
      const eps = entry ? (isSeries ? entry.item.episodes.filter((e) => e.season === entry.ep.season) : entry.item.episodes) : [];
      list = eps.map((ep) => epRow(entry.lib, entry.item, ep, ep.path === P.path ? '播放中' : null)).join('');
    } else if (U.tab === 'subs') {
      const tracks = (P.info && P.info.subtitles) || [];
      const side = entry ? entry.ep.subtitles : [];
      list = '<button class="lan-opt' + (P.sub.source === 'off' ? ' on' : '') + '" data-action="subtitle-off">關閉</button>' +
        tracks.map((t) => '<button class="lan-opt' + (P.sub.source === 'embedded' && P.sub.index === t.index ? ' on' : '') + '" data-action="subtitle-track" data-i="' + t.index + '">' + escH(trackLabel(t)) + (t.text === false ? '（電腦限定）' : '') + '</button>').join('') +
        side.map((s, i) => '<button class="lan-opt' + (P.sub.source === 'external' && P.sub.path === s ? ' on' : '') + '" data-action="subtitle-file" data-i="' + i + '">' + escH(basename(s)) + '</button>').join('');
    } else {
      list = ((p && p.audio) || []).map((t) => '<button class="lan-opt' + (t.index === Player.audio ? ' on' : '') + '" data-action="lan-audio" data-i="' + t.index + '">' +
        escH([LANGS[(t.lang || '').toLowerCase()] || t.lang || '未標示', t.title, String(t.codec).toUpperCase(), t.channels + 'ch'].filter(Boolean).join(' · ')) + '</button>').join('') || '<p class="note">只有一條音軌</p>';
    }
    box.innerHTML = '<div class="lan-under-head"><b>' + escH(entry ? (isSeries ? entry.item.title + ' · ' + entry.ep.label.replace(' · ', '') : entry.ep.title) : basename(P.path)) + '</b>' +
      '<div class="note">原始檔串流' + (info ? ' · ' + escH(info) : '') + '</div></div>' +
      '<div class="lan-seg">' + tabs.map(([v, l]) => '<button class="' + (U.tab === v ? 'on' : '') + '" data-action="lan-under-tab" data-v="' + v + '">' + l + '</button>').join('') + '</div>' +
      '<div class="lan-under-list">' + list + '</div>';
  }
  LAN.renderUnder = renderUnder;

  // ================================================================= phone layout A
  const ICONS = {
    home: '<svg width="22" height="22" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M3 11l9-7 9 7"/><path d="M5 10v10h14V10"/></svg>',
    movie: '<svg width="22" height="22" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linejoin="round"><rect x="3" y="4" width="18" height="16" rx="2"/><path d="M7 4v16M17 4v16M3 9h4M3 15h4M17 9h4M17 15h4"/></svg>',
    series: '<svg width="22" height="22" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linejoin="round"><rect x="3" y="3" width="7" height="7" rx="1.5"/><rect x="14" y="3" width="7" height="7" rx="1.5"/><rect x="3" y="14" width="7" height="7" rx="1.5"/><rect x="14" y="14" width="7" height="7" rx="1.5"/></svg>',
    live: '<svg width="22" height="22" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round"><circle cx="12" cy="12" r="2.5"/><path d="M16.2 7.8a6 6 0 0 1 0 8.4M7.8 16.2a6 6 0 0 1 0-8.4M19 5a10 10 0 0 1 0 14M5 19A10 10 0 0 1 5 5"/></svg>',
    search: '<svg width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.2" stroke-linecap="round"><circle cx="11" cy="11" r="7"/><path d="M20 20l-3.5-3.5"/></svg>',
    sort: '<svg width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.2" stroke-linecap="round" stroke-linejoin="round"><path d="M7 4v16M3 16l4 4 4-4M17 20V4M13 8l4-4 4 4"/></svg>',
    libs: '<svg width="22" height="22" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linejoin="round"><rect x="3" y="3" width="7" height="7" rx="1.5"/><rect x="14" y="3" width="7" height="7" rx="1.5"/><rect x="3" y="14" width="7" height="7" rx="1.5"/><rect x="14" y="14" width="7" height="7" rx="1.5"/></svg>',
    more: '<svg width="22" height="22" viewBox="0 0 24 24" fill="currentColor"><circle cx="5" cy="12" r="2"/><circle cx="12" cy="12" r="2"/><circle cx="19" cy="12" r="2"/></svg>',
    searchTab: '<svg width="22" height="22" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round"><circle cx="11" cy="11" r="7"/><path d="M20 20l-3.5-3.5"/></svg>',
    gear: '<svg width="20" height="20" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><circle cx="12" cy="12" r="3"/><path d="M19.4 15a1.7 1.7 0 0 0 .3 1.8l.1.1a2 2 0 1 1-2.8 2.8l-.1-.1a1.7 1.7 0 0 0-1.8-.3 1.7 1.7 0 0 0-1 1.5V21a2 2 0 1 1-4 0v-.1a1.7 1.7 0 0 0-1.1-1.5 1.7 1.7 0 0 0-1.8.3l-.1.1a2 2 0 1 1-2.8-2.8l.1-.1a1.7 1.7 0 0 0 .3-1.8 1.7 1.7 0 0 0-1.5-1H3a2 2 0 1 1 0-4h.1a1.7 1.7 0 0 0 1.5-1.1 1.7 1.7 0 0 0-.3-1.8l-.1-.1a2 2 0 1 1 2.8-2.8l.1.1a1.7 1.7 0 0 0 1.8.3H9a1.7 1.7 0 0 0 1-1.5V3a2 2 0 1 1 4 0v.1a1.7 1.7 0 0 0 1 1.5 1.7 1.7 0 0 0 1.8-.3l.1-.1a2 2 0 1 1 2.8 2.8l-.1.1a1.7 1.7 0 0 0-.3 1.8V9a1.7 1.7 0 0 0 1.5 1H21a2 2 0 1 1 0 4h-.1a1.7 1.7 0 0 0-1.5 1z"/></svg>',
    chevUp: '<svg width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.6" stroke-linecap="round" stroke-linejoin="round"><path d="M6 15l6-6 6 6"/></svg>',
    caret: '<svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="3" stroke-linecap="round" stroke-linejoin="round"><path d="M6 9l6 6 6-6"/></svg>',
    back: '<svg width="22" height="22" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.4" stroke-linecap="round" stroke-linejoin="round"><path d="M15 18l-6-6 6-6"/></svg>',
  };
  const libIcon = (lib) => ICONS[lib.kind] || ICONS.movie;
  const stillOf = (item, ep) => (ep && ep.thumb) || item.landscape || item.backdrop || item.fanart || '';
  const bgArt = (item, lib, seed) => '<span class="art" style="background:' + art(seed || item.title, lib.kind, 70, 38) + '"></span>';
  const img = (url) => (url ? imgTag(LAN.url(url)) : '');

  // Bottom navigation: always four fixed destinations (Home / Search / Libraries / More), no matter how many libraries the server has.
  function tabBar() {
    const r = S.route;
    const cur = r.name === 'home' ? 'home' : r.name === 'search' ? 'search' : r.name === 'category' || r.name === 'detail' ? 'libs' : 'home';
    const tab = (id, icon, label, action) => '<button class="' + (cur === id ? 'on' : '') + '" data-action="' + action + '">' + icon + '<span>' + label + '</span></button>';
    return '<nav class="lan-tabs" aria-label="主選單">' + tab('home', ICONS.home, '首頁', 'home') + tab('search', ICONS.searchTab, '搜尋', 'lan-search') +
      tab('libs', ICONS.libs, '媒體庫', 'lan-libs') + '<button data-action="lan-settings">' + ICONS.gear + '<span>設定</span></button></nav>';
  }
  LAN.subChoice = {}; // episode path -> 'off' | 'f:<sidecar path>' picked on the detail page, read by autoSelectSubtitles()
  LAN.ownTheme = () => { const v = store.get('vp_lan_theme', ''); return THEMES.some((x) => x[0] === v) ? v : ''; };
  // Only Home keeps the tab bar on screen; every other page sinks it behind a grip (tap it, or swipe up from the bottom edge).
  const dockHtml = () => {
    if (S.route.name === 'home') return tabBar();
    return tabBar().replace('class="lan-tabs"', 'class="lan-tabs sunk"') + '<button class="lan-dock-grip" data-action="lan-dock" aria-label="開啟選單">' + ICONS.chevUp + '</button><div class="lan-dock-shield" data-action="lan-dock-close"></div>';
  };
  function initDock() {
    if (LAN._dock) return;
    LAN._dock = true;
    let y0 = null;
    document.addEventListener('touchstart', (e) => { y0 = e.touches.length === 1 && e.touches[0].clientY > window.innerHeight - 56 ? e.touches[0].clientY : null; }, { passive: true });
    document.addEventListener('touchend', (e) => {
      if (y0 == null) return;
      const dy = e.changedTouches[0].clientY - y0;
      y0 = null;
      if (dy < -28) LAN.actions['lan-dock'](true);
    }, { passive: true });
    // Tapping anywhere else while the home capsule is open just folds it away.
    document.addEventListener('click', (e) => {
      if (!e.target.closest('#lan-cap')) closeCap();
    }, true);
    // Edge swipe (from the left or right screen edge, inward) = back.
    let edge = null;
    document.addEventListener('touchstart', (e) => {
      const t = e.touches[0];
      edge = e.touches.length === 1 && !P.open && (t.clientX < 24 || t.clientX > window.innerWidth - 24) ? { x: t.clientX, y: t.clientY, left: t.clientX < 24 } : null;
    }, { passive: true });
    document.addEventListener('touchend', (e) => {
      if (!edge) return;
      const t = e.changedTouches[0], dx = (t.clientX - edge.x) * (edge.left ? 1 : -1), dy = Math.abs(t.clientY - edge.y);
      edge = null;
      if (dx > 70 && dy < dx * 0.6) LAN.handleBack();
    }, { passive: true });
  }
  // One "back" for the edge swipe and the Android system back: sheet, open dock, then the previous page. false = nothing to go back to.
  LAN.handleBack = function () {
    if (document.getElementById('lan-sheet')) { closeSheet(); return true; }
    const nav = document.querySelector('.lan-tabs.sunk.open');
    if (nav) { nav.classList.remove('open'); return true; }
    const cap = document.getElementById('lan-cap');
    if (cap && cap.classList.contains('open')) { closeCap(); return true; }
    if (P.open) { closePlayer(); return true; }
    if (S.route.name !== 'home') { back(); return true; }
    return false;
  };
  // Single-line titles: the ones that do not fit scroll sideways (reduced-motion users get a plain cut-off instead).
  const marquee = (text, cls) => '<span class="lan-mq' + (cls ? ' ' + cls : '') + '"><span>' + escH(text) + '</span></span>';
  function setupMarquee(root) {
    const calm = window.matchMedia('(prefers-reduced-motion: reduce)').matches;
    root.querySelectorAll('.lan-mq').forEach((box) => {
      const inner = box.firstElementChild;
      const over = inner.scrollWidth - box.clientWidth;
      if (over > 2 && !calm && !box.closest('.open')) {
        box.style.setProperty('--mq-x', -(over + 4) + 'px');
        box.style.setProperty('--mq-t', Math.max(6, over / 28 + 3).toFixed(1) + 's');
        box.classList.add('run');
      } else box.classList.remove('run');
    });
  }
  // Bottom sheet (library picker, season picker, more).
  function openSheet(title, body) {
    closeSheet();
    const box = document.createElement('div');
    box.id = 'lan-sheet';
    box.className = 'lan-sheet';
    box.innerHTML = '<div class="lan-sheet-bg" data-action="lan-sheet-close"></div><div class="lan-sheet-panel" role="dialog" aria-label="' + escH(title) + '"><div class="lan-sheet-grip"></div><b class="lan-sheet-title">' + escH(title) + '</b>' + body + '</div>';
    document.body.appendChild(box);
  }
  function closeSheet() { const box = document.getElementById('lan-sheet'); if (box) box.remove(); }
  // One capsule per library (no "all" chip); the open library is highlighted.
  const libChips = (current) => '<div class="lan-chips libchips">' +
    S.libs.map((lib) => '<button class="' + (current === lib.id ? 'on' : '') + '" data-action="lan-lib" data-lib="' + escH(lib.id) + '">' + escH(lib.name) + '</button>').join('') + '</div>';
  function topBar(title, extra) {
    return '<header class="lan-top"><h1>' + escH(title) + '</h1>' + (extra || '') + '</header>';
  }
  const pair = (buttons) => '<div class="lan-capsule">' + buttons + '</div>';
  const searchBtn = '<button class="lan-pi" data-action="lan-search" aria-label="搜尋">' + ICONS.search + '</button>';
  const settingsBtn = '<button class="lan-pi" data-action="lan-settings" aria-label="設定">' + ICONS.gear + '</button>';
  // Home header: the mark opens a capsule with search + settings over the library chips; the chips share its row and scroll sideways.
  const homeBar = () => '<header class="lan-top home"><div class="lan-capsule dock" id="lan-cap"><button class="lan-mark" data-action="lan-cap" aria-label="選單" aria-expanded="false">' +
    '<img class="m-light" src="brand/mark.svg" alt=""><img class="m-dark" src="brand/mark-crimson.svg" alt=""></button><div class="lan-cap-actions">' + searchBtn + settingsBtn + '</div></div>' +
    (S.scanned ? libChips('') : '') + '</header>';
  const closeCap = () => { const cap = document.getElementById('lan-cap'); if (!cap) return; cap.classList.remove('open'); cap.parentElement.classList.remove('cap-open'); const b = cap.querySelector('.lan-mark'); if (b) b.setAttribute('aria-expanded', 'false'); };

  function wideCard(entry, sub) {
    const { lib, item, ep } = entry;
    const pct = pctOf(ep);
    return '<button class="lan-wide" data-action="play-path" data-path="' + escH(ep.path) + '">' +
      '<span class="lan-img">' + bgArt(item, lib, item.title + ep.path) + resumeArt(ep, img(stillOf(item, ep))) + '</span>' +
      '<span class="lan-cap"><b>' + escH(lib.kind === 'series' ? item.title + ' ' + ep.label.replace(' · ', '') : ep.title) + '</b><span>' + escH(sub) + '</span></span>' +
      (pct ? '<span class="lan-bar"><i style="width:' + pct + '%"></i></span>' : '') + '</button>';
  }
  function posterTile(lib, item, small) {
    const poster = posterOf(item);
    const seasons = lib.kind === 'series' ? item.seasons.length + ' 季 · ' + item.episodes.length + ' 集' : (item.year || '');
    return '<button class="lan-poster' + (small ? ' small' : '') + '" data-action="detail" data-lib="' + escH(lib.id) + '" data-item="' + escH(item.id) + '">' +
      '<span class="lan-img">' + bgArt(item, lib) + img(poster) + '</span>' +
      '<span class="lan-ttl">' + escH(item.title) + '</span>' + (small ? '' : '<span class="lan-meta">' + escH(seasons) + '</span>') + '</button>';
  }

  function phoneHome() {
    const cont = [];
    for (const lib of S.libs) cont.push(...homeEntries(lib, false).map((e) => Object.assign({ at: progressOf(e.ep).updated }, e)));
    cont.sort((a, b) => b.at - a.at);
    let html = homeBar();
    if (!S.scanned) return html + '<p class="lan-empty">正在讀取媒體庫…</p>';
    if (cont.length) {
      const first = cont[0], pct0 = pctOf(first.ep);
      const title0 = first.lib.kind === 'series' ? first.item.title + ' ' + first.ep.label.replace(' · ', '') : first.ep.title;
      html += '<button class="lan-feature" data-action="play-path" data-path="' + escH(first.ep.path) + '" aria-label="繼續播放 ' + escH(title0) + '"><span class="lan-img">' + bgArt(first.item, first.lib, first.item.title + first.ep.path) + resumeArt(first.ep, img(stillOf(first.item, first.ep))) + '</span>' +
        '<span class="lan-feature-play" aria-hidden="true">' + ICON.play + '</span>' +
        '<span class="lan-feature-tx">' + marquee(title0, 'feat') + (pct0 ? '<span class="lan-bar static"><i style="width:' + pct0 + '%"></i></span>' : '') + '</span></button>';
      if (cont.length > 1) html += '<div class="lan-sec"><b>繼續播放</b><span>' + (cont.length - 1) + '</span></div><div class="lan-row">' +
        cont.slice(1, 20).map((e) => { const p = progressOf(e.ep); return wideCard(e, '剩 ' + minutesLeft(p.dur - p.pos) + ' 分'); }).join('') + '</div>';
    }
    for (const lib of S.libs) {
      if (!lib.items.length) continue;
      const items = lib.items.slice().sort((a, b) => (b.mtime || 0) - (a.mtime || 0)).slice(0, 15);
      html += '<div class="lan-sec"><b>最近新增 · ' + escH(lib.name) + '</b><button data-action="lan-lib" data-lib="' + escH(lib.id) + '">全部 ›</button></div>' +
        '<div class="lan-row">' + items.map((item) => posterTile(lib, item, true)).join('') + '</div>';
    }
    if (!S.libs.some((l) => l.items.length)) html += '<p class="lan-empty">伺服器的媒體庫是空的</p>';
    return html;
  }

  function phoneCategory(libId) {
    const lib = S.libs.find((l) => l.id === libId);
    if (!lib) return phoneHome();
    const filter = S.prefs.lanFilter || 'all';
    let items = sortedItems(lib);
    if (filter === 'partial') items = items.filter((i) => i.episodes.some((e) => { const p = progressOf(e); return p && !p.done && p.pos > 15000; }));
    else if (filter === 'unwatched') items = items.filter((i) => !i.episodes.some((e) => progressOf(e)));
    const sortLabel = SORTS[S.cat.sort] || SORTS[0];
    return topBar(lib.name, pair(searchBtn + '<button class="lan-pi" data-action="lan-sort" aria-label="排序：' + escH(sortLabel) + '" title="' + escH(sortLabel) + '">' + ICONS.sort + '</button>')) +
      '<div class="lan-chips">' + [['all', '全部'], ['partial', '看到一半'], ['unwatched', '未看']].map(([v, l]) => '<button class="' + (filter === v ? 'on' : '') + '" data-action="lan-filter" data-v="' + v + '">' + l + '</button>').join('') + '</div>' +
      '<div class="lan-note-row">' + escH(sortLabel) + ' · ' + items.length + ' 部</div>' +
      (items.length ? '<div class="lan-grid">' + items.map((item) => posterTile(lib, item)).join('') + '</div>' : '<p class="lan-empty">沒有符合的作品</p>');
  }

  // Phase 3 (3-7, D3): an episode synopsis in another language than the user's is marked 〔日文〕 (as on the desktop)
  const lanEpNote = (ep) => { const n = typeof epPlotNote === 'function' ? epPlotNote(ep) : ''; return n ? '〔' + n + '〕' : ''; };
  // Phase 3B (3B-7): an episode's labels as on the desktop (總集篇／特別篇／資料庫沒有／已自訂: app.js epovChips over ep.epov, set
  // by the same readers from the same file); display only: the phone rows have no menu.
  const lanEpChips = (ep) => { const c = typeof epovChips === 'function' ? epovChips(ep.epov) : []; return c.length ? '<span class="lan-ep-chips">' + c.map((t) => '<em class="ep-chip' + (t === '已自訂' ? '' : ' mark') + '">' + escH(t) + '</em>').join('') + '</span>' : ''; };
  function epRow(lib, item, ep, label) {
    const p = progressOf(ep);
    const pct = pctOf(ep);
    const sub = label || (p && p.done ? '已看完' : p && p.dur && p.pos > 15000 ? '剩 ' + minutesLeft(p.dur - p.pos) + ' 分' : ep.meta && ep.meta.runtime ? ep.meta.runtime + ' 分' : '');
    const title = lib.kind === 'series' ? (ep.episode != null ? ep.episode + '. ' : '') + ep.title : ep.file;
    return '<button class="lan-ep' + (ep.path === P.path && P.open ? ' cur' : '') + '" data-action="play-path" data-path="' + escH(ep.path) + '">' +
      '<span class="lan-img">' + bgArt(item, lib, item.title + ep.path) + resumeArt(ep, img(stillOf(item, ep))) + (pct ? '<span class="lan-bar"><i style="width:' + pct + '%"></i></span>' : '') + '</span>' +
      '<span class="lan-ep-tx"><b>' + escH(title) + '</b>' + lanEpChips(ep) + '<span>' + escH(sub) + '</span>' + (ep.meta && ep.meta.plot ? '<span class="lan-ep-plot">' + escH(lanEpNote(ep) + ep.meta.plot) + '</span>' : '') + '</span></button>';
  }

  // Phase 3 (3-3): the synopsis block with the desktop's semantics (app.js seasonSynopsis, read only): the season synopsis
  // with its source, then the work synopsis — below it when it describes this season's work, folded into a one-line
  // 「作品簡介 · …」 button when it belongs to another work / edition (never replaced by the season's); with only another
  // language for the season, 「第 N 季 · 此季沒有中文簡介（…）」 above the work synopsis. Films: the work synopsis as before.
  // -> {main: {text, label} | null, work: {text, label, folded, key} | null, note}
  const lanWorkOpen = new Set(); // folded work synopses opened on this device (memory only)
  function phonePlotView(item, season, isSeries) {
    const m = item.meta || {};
    if (!isSeries || typeof seasonSynopsis !== 'function') return { main: m.plot ? { text: m.plot, label: '' } : null, work: null, note: '' };
    const v = seasonSynopsis(item, season);
    const lang = v.season && typeof synLangNote === 'function' ? synLangNote(v.season.locale) : '';
    const work = v.work ? { text: v.work.text, label: v.collapsed ? v.foldLabel || '作品簡介 · ' + v.work.label : '作品簡介 · 簡介來源：' + (v.work.src || '本機 .nfo'), folded: !!v.collapsed, key: item.id + ':' + season } : null;
    if (v.season) return { main: { text: v.season.text, label: '第 ' + season + ' 季 · 簡介來源：' + v.season.src + (lang ? ' · ' + lang : '') }, work, note: '' };
    return { main: work ? { text: work.text, label: work.label } : null, work: null, note: v.note ? '第 ' + season + ' 季 · ' + v.note : '' };
  }
  function phonePlotHtml(view) {
    let h = view.note ? '<p class="lan-plot-src">' + escH(view.note) + '</p>' : '';
    if (view.main) h += (view.main.label ? '<p class="lan-plot-src">' + escH(view.main.label) + '</p>' : '') +
      '<p class="lan-plot" data-action="lan-plot">' + escH(view.main.text) + '</p><button class="lan-more-link" data-action="lan-plot">展開</button>';
    const w = view.work;
    if (w && !w.folded) h += '<p class="lan-plot-src">' + escH(w.label) + '</p><p class="lan-plot open lan-plot-work">' + escH(w.text) + '</p>';
    else if (w) {
      const open = lanWorkOpen.has(w.key);
      h += '<button class="lan-work-btn' + (open ? ' open' : '') + '" data-action="lan-work-plot" data-key="' + escH(w.key) + '" aria-expanded="' + open + '">' + escH(w.label) + '</button>' +
        (open ? '<p class="lan-plot open lan-plot-work">' + escH(w.text) + '</p>' : '');
    }
    return h;
  }

  function phoneDetail(libId, itemId) {
    const { lib, item } = findItem(libId, itemId);
    if (!item) return topBar('找不到這個項目') + '<p class="lan-empty">媒體庫可能已重新掃描。</p>';
    const isSeries = lib.kind === 'series';
    const resume = resumeEpisode(item);
    const rp = progressOf(resume);
    // season 0 (特別篇) is a season: tested with != null, as on the desktop detail page (renderDetail)
    if (S.detail.season == null || !item.seasons.includes(S.detail.season)) S.detail.season = S.route.season != null && item.seasons.includes(S.route.season) ? S.route.season : resume.season;
    const season = S.detail.season;
    const eps = isSeries ? item.episodes.filter((e) => e.season === season) : item.episodes;
    const m = item.meta || {};
    wantEmbedded(item);
    const label = (rp && !rp.done && rp.pos > 15000 ? '繼續 ' : '播放 ') + (isSeries ? resume.label.replace(' · ', '') : '');
    const tags = [item.year, m.genres && m.genres.slice(0, 2).join('／'), m.tmdbScore ? 'TMDB ' + (+m.tmdbScore).toFixed(1) : null, m.alScore ? 'AniList ' + m.alScore : null,
      m.bgmScore ? 'Bangumi ' + (+m.bgmScore).toFixed(1) : null, isSeries ? item.seasons.length + ' 季' : m.runtime ? m.runtime + ' 分鐘' : null].filter(Boolean);
    const hero = item.backdrop || item.fanart || item.landscape || posterOf(item, isSeries ? season : null);
    const plot = phonePlotHtml(phonePlotView(item, season, isSeries)); // Phase 3 (3-3): desktop semantics, not a replacement
    const resumed = rp && rp.pos > 15000 && !rp.done;
    const chapterMode = !isSeries; // movies and every other non-series library (e.g. Others) share the movie layout
    let lower;
    if (chapterMode) {
      const ep = eps.find((e) => e.path === S.detail.segPath) || resume;
      const files = eps.length > 1 ? '<div class="lan-seg scroll-x">' + eps.map((e, i) => '<button class="' + (e === ep ? 'on' : '') + '" data-action="movie-file" data-path="' + escH(e.path) + '">' + escH(e.title && e.title !== item.title ? e.title : '檔案 ' + (i + 1)) + '</button>').join('') + '</div>' : '';
      lower = files + chapterList(ep);
    } else {
      lower = (isSeries && item.seasons.length > 1 ? '<button class="lan-season" data-action="lan-season-sheet">第 ' + season + ' 季 ' + ICONS.caret + '</button>' : '') +
        '<div class="lan-sec"><b>' + (isSeries ? '集數' : '檔案') + '</b><span>' + eps.length + '</span></div>' +
        '<div class="lan-eps">' + eps.map((ep) => epRow(lib, item, ep)).join('') + '</div>';
    }
    const subtitle = [m.chineseTitle && m.chineseTitle !== item.title ? m.chineseTitle : '', tags.join(' · ')].filter(Boolean).join(' · ');
    // Phase 1B-a γ4: the host's review marker, shown read-only (no action on a client)
    const review = typeof alReviewBadge === 'function' && alReviewBadge(lib, item) ? '<span class="lan-review" title="' + escH(alReviewTitle(alReviewOf(item))) + '">比對待複查</span>' : '';
    return '<div class="lan-hero">' + bgArt(item, lib) + img(hero) + '<span class="lan-hero-fade"></span>' +
      '<div class="lan-hero-tx" data-action="lan-title-toggle"><h1>' + marquee(item.title, 'big') + '</h1><div class="lan-hero-meta">' + escH(subtitle) + review + '</div></div></div>' +
      '<div class="lan-detail">' +
        '<div class="lan-actions"><button class="btn pri" data-action="play-path" data-path="' + escH(resume.path) + '">' + ICON.play + escH(label.trim()) + (resumed && rp.dur ? '　剩 ' + minutesLeft(rp.dur - rp.pos) + ' 分' : '') + '</button>' +
          '<button class="lan-ic lan-act-ic" data-action="lan-detail-menu" aria-label="更多選項" title="更多選項">' + ICONS.more + '</button></div>' +
        (resumed && rp.dur ? '<div class="lan-prog"><span class="lan-bar static"><i style="width:' + pctOf(resume) + '%"></i></span></div>' : '') +
        plot +
      '</div>' + lower;
  }

  // Chapters of one file: a vertical list, one row per interval, with frames made on demand as rows scroll into view.
  function chapterList(ep) {
    const interval = chapterInterval();
    const t = thumbsOf(ep.path);
    const duration = durationOf(ep);
    const key = hash(ep.path).toString(36);
    const chips = '<div class="lan-seg scroll-x">' + SEG_INTERVALS.map(([v, l]) => '<button class="' + (interval === v ? 'on' : '') + '" data-action="seg-interval" data-v="' + v + '">' + l + '</button>').join('') + '</div>';
    if (!duration) return '<div class="lan-sec"><b>章節</b></div>' + chips + '<p class="lan-empty">正在讀取影片長度…</p>';
    const count = Math.ceil(duration / interval);
    const p = progressOf(ep);
    const rows = [];
    for (let i = 0; i < count; i++) {
      const start = i * interval, end = Math.min(duration, start + interval);
      const here = p && !p.done && p.pos >= start && p.pos < end;
      const url = t.exact[start];
      rows.push('<button class="lan-chap' + (here ? ' cur' : '') + '" data-action="play-path" data-path="' + escH(ep.path) + '" data-start="' + start + '">' +
        '<span class="lan-img" data-seg-key="' + key + '" data-seg-ms="' + start + '">' + (url ? '<img src="' + escH(url) + '" alt="">' : '<span class="seg-wait"></span>') + '</span>' +
        '<span class="lan-ep-tx"><b>第 ' + (i + 1) + ' 章</b><span>' + clock(start) + ' – ' + clock(end) + (here ? ' · 上次看到這裡' : '') + '</span></span></button>');
    }
    return '<div class="lan-sec"><b>章節</b><span>' + count + '</span></div>' + chips +
      '<div class="lan-chaps" data-path="' + escH(ep.path) + '" data-interval="' + interval + '">' + rows.join('') + '</div>';
  }
  // Ask the host for the frames of the rows on screen (a newer batch replaces the running one).
  function requestChapterFrames() {
    const box = document.querySelector('.lan-chaps');
    if (!box) return;
    const path = box.dataset.path, interval = +box.dataset.interval;
    const t = thumbsOf(path);
    const limit = window.innerHeight + 500;
    const times = [];
    box.querySelectorAll('.lan-chap').forEach((row) => {
      const r = row.getBoundingClientRect();
      if (r.bottom < -300 || r.top > limit) return;
      const ms = +row.dataset.start;
      if (!t.exact[ms] && times.length < 14) times.push(ms);
    });
    if (!times.length || times.every((ms) => t.inflight.has(ms))) return;
    t.inflight = new Set(times);
    send('thumbnailsAt', { slot: 'segments', path, times, tag: 'chapters' });
  }

  function phoneSearch() {
    const q = (S.query || '').trim().toLowerCase();
    const hits = [];
    if (q) S.libs.forEach((lib) => lib.items.forEach((item) => {
      const names = [item.title, item.meta && item.meta.chineseTitle, item.meta && item.meta.originaltitle].filter(Boolean).map((t) => t.toLowerCase());
      if (names.some((t) => t.includes(q)) || item.episodes.some((e) => (e.title || '').toLowerCase().includes(q))) hits.push(posterTile(lib, item));
    }));
    return '<header class="lan-top"><button class="lan-ic" data-action="lan-back" aria-label="返回">' + ICONS.back + '</button>' +
      '<input class="lan-search" id="lan-q" type="search" placeholder="搜尋影片、劇集" value="' + escH(S.query || '') + '" autocomplete="off"></header>' +
      (q ? (hits.length ? '<div class="lan-grid">' + hits.join('') + '</div>' : '<p class="lan-empty">沒有符合的結果</p>') : '<p class="lan-empty">輸入片名的一部分</p>');
  }

  // Called by app.js render(); true = this file drew the page.
  LAN.render = function () {
    if (!LAN.phone) {
      decorateDesktop();
      return false;
    }
    try { if (window.VPShell && window.VPShell.setTheme) window.VPShell.setTheme(phoneTheme(LAN.ownTheme() || themeConf(S.config))); } catch { /* not in the Android shell */ }
    const view = document.getElementById('view');
    const r = S.route;
    let html;
    if (r.name === 'category') html = phoneCategory(r.lib);
    else if (r.name === 'detail') html = phoneDetail(r.lib, r.item);
    else if (r.name === 'search') html = phoneSearch();
    else html = phoneHome();
    view.innerHTML = '<div class="lan-page' + (r.name === 'detail' ? ' is-detail' : '') + (r.name === 'home' || r.name === 'search' ? '' : ' docked') + '">' + html + '</div>' + (r.name === 'search' ? '' : dockHtml());
    initDock();
    setupMarquee(view);
    if (document.fonts && document.fonts.ready) document.fonts.ready.then(() => setupMarquee(view));
    if (r.name === 'detail') {
      const page = view.querySelector('.lan-page');
      const { lib, item } = findItem(r.lib, r.item);
      if (item && lib.kind !== 'series') {
        const ep = item.episodes.find((e) => e.path === S.detail.segPath) || resumeEpisode(item);
        if (!durationOf(ep)) requestDuration(ep.path);
        else {
          if (page) page.addEventListener('scroll', () => { clearTimeout(page._chap); page._chap = setTimeout(requestChapterFrames, 120); }, { passive: true });
          requestAnimationFrame(requestChapterFrames);
        }
      }
    }
    if (r.name === 'search') {
      const input = document.getElementById('lan-q');
      if (input) {
        if (document.activeElement !== input) { input.focus(); input.setSelectionRange(input.value.length, input.value.length); }
        input.addEventListener('input', () => { S.query = input.value; clearTimeout(input._t); input._t = setTimeout(() => { const pos = input.selectionStart; render(); const again = document.getElementById('lan-q'); if (again) { again.focus(); again.setSelectionRange(pos, pos); } }, 250); });
      }
    }
    requestResumeFrames();
    pumpEmbedded();
    return true;
  };

  // Desktop-sized clients: the normal UI, plus a connection chip.
  function decorateDesktop() {
    const right = document.querySelector('.top-right');
    if (right && !right.querySelector('.lan-chip')) {
      right.insertAdjacentHTML('afterbegin', '<button class="glass lan-chip big" data-action="lan-server" title="' + (nativeHost ? '切換伺服器' : '已連線') + '"><i></i>' + escH(LAN.serverName) + '</button>');
    }
  }

  LAN.actions = {
    'lan-lib': (d) => { closeSheet(); S.cat.sort = 0; S.prefs.lanLib = d.lib; go({ name: 'category', lib: d.lib }); },
    'lan-libs': () => {
      if (S.libs.length === 1) { LAN.actions['lan-lib']({ lib: S.libs[0].id }); return; }
      const last = S.route.lib || S.prefs.lanLib;
      openSheet('媒體庫', S.libs.map((lib) => '<button class="lan-sheet-item' + (lib.id === last ? ' on' : '') + '" data-action="lan-lib" data-lib="' + escH(lib.id) + '">' + libIcon(lib) + '<span>' + escH(lib.name) + '</span><em>' + lib.items.length + '</em></button>').join(''));
    },
    'lan-settings': () => {
      closeCap();
      const cur = phoneTheme(LAN.ownTheme() || themeConf(S.config));
      const stream = streamPreferences();
      const streamToggle = (action, label, enabled) =>
        '<button class="lan-sheet-item' + (enabled ? ' on' : '') + '" data-action="' + action + '"><span>' + label +
        '</span><em>' + (enabled ? '開啟' : '關閉') + '</em></button>';
      openSheet('設定',
        '<b class="lan-sheet-sub">主題</b>' + THEMES.filter(([v]) => phoneTheme(v) === v).map(([v, name]) => '<button class="lan-sheet-item' + (cur === v ? ' on' : '') + '" data-action="lan-theme" data-v="' + v + '"><span>' + escH(name) + '</span></button>').join('') +
        '<b class="lan-sheet-sub">此裝置的串流處理</b>' +
        streamToggle('lan-stream-upscale', '伺服器超分', stream.upscale) +
        streamToggle('lan-stream-interpolation', '伺服器補幀', stream.interpolation) +
        '<p class="lan-sheet-note">處理使用伺服器目前設定的演算法與模型；變更於下次播放生效。測試串流目前沒有音訊，一次支援一條處理串流。</p>' +
        '<b class="lan-sheet-sub">連線</b><div class="lan-sheet-kv"><span>伺服器</span><b>' + escH(LAN.serverName) + '</b></div><div class="lan-sheet-kv"><span>狀態</span><b>已連線</b></div>' +
        '<button class="lan-sheet-item danger" data-action="lan-logout"><span>登出這台裝置</span></button>');
    },
    'lan-stream-upscale': () => {
      const stream = streamPreferences();
      stream.upscale = !stream.upscale;
      saveStreamPreferences(stream);
      LAN.actions['lan-settings']();
    },
    'lan-stream-interpolation': () => {
      const stream = streamPreferences();
      stream.interpolation = !stream.interpolation;
      saveStreamPreferences(stream);
      LAN.actions['lan-settings']();
    },
    'lan-theme': (d) => { store.set('vp_lan_theme', d.v); applyTheme(); LAN.actions['lan-settings'](); },
    'lan-dock': (force) => { const nav = document.querySelector('.lan-tabs.sunk'); if (nav) nav.classList.toggle('open', force === true ? true : !nav.classList.contains('open')); },
    'lan-dock-close': () => { const nav = document.querySelector('.lan-tabs.sunk'); if (nav) nav.classList.remove('open'); },
    'lan-cap': () => {
      const cap = document.getElementById('lan-cap');
      if (!cap) return;
      const open = cap.classList.toggle('open');
      cap.parentElement.classList.toggle('cap-open', open);
      const b = cap.querySelector('.lan-mark');
      if (b) b.setAttribute('aria-expanded', String(open));
    },
    'lan-title-toggle': (d, node) => { const box = document.querySelector('.lan-hero-tx'); if (box) { box.classList.toggle('open'); setupMarquee(box); } },
    'lan-detail-menu': () => {
      const { lib, item } = findItem(S.route.lib, S.route.item);
      if (!item) return;
      const ep = resumeEpisode(item), rp = progressOf(ep);
      const subs = ep.subtitles || [];
      const pick = LAN.subChoice[ep.path] || 'auto';
      const opt = (v, label) => '<button class="lan-sheet-item' + (pick === v ? ' on' : '') + '" data-action="lan-sub-pick" data-v="' + escH(v) + '"><span>' + escH(label) + '</span></button>';
      openSheet('更多選項',
        (rp && rp.pos > 15000 && !rp.done ? '<button class="lan-sheet-item" data-action="lan-restart" data-path="' + escH(ep.path) + '"><span>從頭播放</span></button>' : '') +
        '<b class="lan-sheet-sub">外掛字幕</b>' + opt('auto', '自動（預設）') + opt('off', '不載入字幕') +
        (subs.length ? subs.map((s, i) => opt('f:' + s, basename(s))).join('') : '<p class="lan-sheet-note">沒有找到外掛字幕檔</p>'));
    },
    'lan-sub-pick': (d) => {
      const { item } = findItem(S.route.lib, S.route.item);
      if (!item) return;
      const ep = resumeEpisode(item);
      if (d.v === 'auto') delete LAN.subChoice[ep.path]; else LAN.subChoice[ep.path] = d.v;
      LAN.actions['lan-detail-menu']();
    },
    'lan-restart': (d) => { closeSheet(); playPath(d.path, 0); },
    'lan-more': () => LAN.actions['lan-settings'](),
    'lan-sheet-close': () => closeSheet(),
    'lan-logout': () => {
      if (!confirm('登出後需要重新輸入 PIN。要登出這台裝置嗎？')) return;
      store.set('vp_lan_token', '');
      document.cookie = 'vp_token=; Max-Age=0; path=/';
      location.reload();
    },
    'lan-season-sheet': () => {
      const m = findItem(S.route.lib, S.route.item);
      if (!m.item) return;
      openSheet('選擇季別', m.item.seasons.map((n) => '<button class="lan-sheet-item' + (n === S.detail.season ? ' on' : '') + '" data-action="lan-season" data-s="' + n + '"><span>第 ' + n + ' 季</span><em>' + m.item.episodes.filter((e) => e.season === n).length + ' 集</em></button>').join(''));
    },
    'lan-season': (d) => { closeSheet(); S.detail.season = +d.s; render(); },
    'lan-back': () => back(),
    'lan-search': () => go({ name: 'search' }),
    'lan-sort': () => { S.cat.sort = (S.cat.sort + 1) % SORTS.length; render(); },
    'lan-filter': (d) => { S.prefs.lanFilter = d.v; savePrefs(); render(); },
    'lan-work-plot': (d) => { if (lanWorkOpen.has(d.key)) lanWorkOpen.delete(d.key); else lanWorkOpen.add(d.key); render(); },
    'lan-plot': (d, node) => { const p = document.querySelector('.lan-plot'); if (p) { const open = p.classList.toggle('open'); const l = document.querySelector('.lan-more-link'); if (l) l.textContent = open ? '收合' : '展開'; } },
    'lan-delete-media': async (d) => {
      if (!LAN.canDelete || !d.path) return;
      const name = String(d.path).split(/[\\/]/).pop();
      if (!confirm('要將「' + name + '」移至伺服器的資源回收筒嗎？')) return;
      try {
        const response = await fetch('/api/media?p=' + encodeURIComponent(d.path), { method: 'DELETE', headers: headers(), credentials: 'same-origin', cache: 'no-store' });
        if (!response.ok) { alert(response.status === 403 ? '沒有刪除權限，或此檔案不在可閱覽的媒體庫中。' : '無法刪除影片。'); return; }
        const current = Object.assign({}, S.route);
        const scan = await call('scan');
        buildLibraries(scan.libraries);
        if (current.name === 'detail' && !findItem(current.lib, current.item).item) go({ name: 'category', lib: current.lib });
        else render();
        toast('影片已移至資源回收筒');
      } catch { alert('無法連線到伺服器，影片尚未重新整理。'); }
    },
    'lan-under-tab': (d) => { U.tab = d.v; renderUnder(); },
    'lan-audio': (d) => { Player.setAudio(+d.i); renderUnder(); },
    'lan-server': () => {
      if (!nativeHost) { toast('已連線到「' + LAN.serverName + '」'); return; }
      if (confirm('要中斷與「' + LAN.serverName + '」的連線並選擇其他伺服器嗎？')) send('lanDisconnect');
    },
  };

  // Keep the portrait "under the video" panel in step with the player.
  if (typeof window !== 'undefined') {
    const hook = () => {
      if (typeof listeners === 'undefined') return setTimeout(hook, 50);
      listeners.thumb.push((m) => { if (m.kind === 'done' && m.exact && m.tag === 'chapters') setTimeout(requestChapterFrames, 60); });
      listeners.player.push((event) => {
        if (nativeHost) return;
        if (event === 'opened') { U.tab = 'eps'; LAN.layoutPlayer(); }
        if ((event === 'opened' || event === 'subtitle') && LAN.portrait()) setTimeout(renderUnder, 0);
      });
    };
    document.addEventListener('DOMContentLoaded', hook);
    // A client never looks things up online itself: it shows what the server already matched.
    document.addEventListener('DOMContentLoaded', () => {
      for (const name of ['alPump', 'tmdbEnsure', 'seasonCfgEnsure']) if (typeof window[name] === 'function') window[name] = () => {};
      if (typeof window.alEnsureDetail === 'function') window.alEnsureDetail = () => Promise.resolve();
      if (typeof window.alFromTmdb === 'function') window.alFromTmdb = () => Promise.resolve(false);
    });
  }
})();
