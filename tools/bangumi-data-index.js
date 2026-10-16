'use strict';
// Phase 5 batch C (R4, C1): the library-scoped bangumi-data index the app ships (ui/vendor/bangumi-data/index.json).
// Offline: reads (1) a PINNED local copy of bangumi-data's dist/data.json (SHA-256 checked first; parsed with JSON.parse only, never
// executed) and (2) a COPY of the metadata store (anilist.json; the live data folder only with --live), writes one file. No network.
// Only the entries the library's EXISTING ids reach are kept (O1: library-scoped; titles are never used to find an entry):
//   - an entry whose aniList / mal / bangumi id is one the store holds (work links, season cells, season settings, lock pins; MAL =
//     the cached AniList entries' idMal), or whose TMDB placement is a film / tv id the store holds;
//   - then every entry placed on the TMDB tv series those entries name (the app computes a placement's episode range from the next
//     entry's start in the same TMDB season, so a series is kept whole).
// Per entry: {i (position in data.json, the app's tie-break), title, type, begin (date), sites: {anilist: [id], mal: [id], bangumi: [id],
// tmdb: [{type, id, season?, ep?}]}} (empty sites left out). Header: {src: 'bangumi-data@<ver>', commit, sha256, license, source,
// generatedAt, n}. Deterministic: entries in data.json order, fixed key order, one entry per line; generatedAt is the library snapshot
// time (the latest matches[*].at of the store) unless --at is given, so the same inputs give the same bytes.
// Usage: node tools/bangumi-data-index.js --dataset <data.json> --store <anilist.json copy> [--out <index.json>] [--at <ISO time>] [--live]
//        [--sha256 <hex>] [--ver <tag>] [--commit <sha>] [--check]   (--check: compare with the existing --out, write nothing; exit 1 if it differs)
//        [--zh]   (Traditional Chinese status lines as well: the .cmd keeps its own text ASCII, since cmd.exe misreads UTF-8 batch lines)
// Exit 0 written (or identical with --check), 1 differs (--check), 2 refused (dataset SHA-256 / schema mismatch, unreadable store, the live
// data folder without --live), nothing written.
// Regenerate when the library grows (new works get dataset candidates only once their ids are in the index): see
// tools/bangumi-data-index.cmd (double-click) and tools/metadata-backup-README.md.
const fs = require('fs');
const path = require('path');
const crypto = require('crypto');

const PIN = { ver: '0.3.231', commit: 'bae940f6116ae4b93c73a711827056faaf23957a', sha256: 'da3ec080c41a2015d658572a70d140556a51fa66e3dde3e18894dcee1974111b' };
const ROOT = path.resolve(__dirname, '..');
const argv = process.argv.slice(2);
const arg = (name, def) => { const i = argv.indexOf('--' + name); return i >= 0 ? argv[i + 1] : def; };
const flag = (name) => argv.includes('--' + name);
const OPT = {
  dataset: arg('dataset', ''),
  store: arg('store', ''),
  out: arg('out', path.join(ROOT, 'ui', 'vendor', 'bangumi-data', 'index.json')),
  at: arg('at', ''),
  sha256: arg('sha256', PIN.sha256),
  ver: String(arg('ver', PIN.ver)).replace(/^v/, ''),
  commit: arg('commit', PIN.commit),
  live: flag('live'),
  check: flag('check'),
  zh: flag('zh'),
};
const zh = (text) => { if (OPT.zh) console.log(text); };
const refuse = (why, zhWhy) => { console.error('[bangumi-data-index] refused: ' + why + ' (nothing written)'); zh('[錯誤] ' + (zhWhy || why) + '；沒有產生索引，原本的索引不變。'); process.exit(2); };
const sha256 = (buf) => crypto.createHash('sha256').update(buf).digest('hex');

function build(datasetBuf, storeText, opt) {
  const dsSha = sha256(datasetBuf);
  if (dsSha !== opt.sha256) throw Object.assign(new Error('dataset SHA-256 ' + dsSha + ' != pinned ' + opt.sha256), { refused: true });
  let DS;
  try { DS = JSON.parse(datasetBuf.toString('utf8')); } catch (e) { throw Object.assign(new Error('dataset is not JSON'), { refused: true }); }
  if (!DS || !Array.isArray(DS.items) || !DS.siteMeta || typeof DS.siteMeta !== 'object') throw Object.assign(new Error('dataset schema mismatch (items[] / siteMeta)'), { refused: true });
  let store;
  try { store = JSON.parse(storeText); } catch (e) { throw Object.assign(new Error('store is not JSON'), { refused: true }); }
  if (!store || typeof store.matches !== 'object' || !store.matches) throw Object.assign(new Error('store has no matches'), { refused: true });

  // the library's existing ids
  const obj = (x) => !!x && typeof x === 'object' && !Array.isArray(x);
  const pos = (v) => (Number.isInteger(+v) && +v > 0 ? +v : 0);
  const AL = new Set(), MAL = new Set(), BGM = new Set(), MOVIE = new Set(), TV = new Set();
  const addTmdb = (t) => { if (obj(t) && pos(t.id)) (t.type === 'movie' ? MOVIE : TV).add(+t.id); };
  let snap = 0;
  for (const m of Object.values(store.matches)) {
    if (!obj(m)) continue;
    if (+m.at > snap) snap = +m.at;
    if (pos(m.id)) AL.add(+m.id);
    if (pos(m.bgm)) BGM.add(+m.bgm);
    addTmdb(m.tmdb);
    for (const v of Object.values(obj(m.seasons) ? m.seasons : {})) if (pos(v)) AL.add(+v);
    for (const v of Object.values(obj(m.seasonBgm) ? m.seasonBgm : {})) if (pos(v)) BGM.add(+v);
    for (const c of Object.values(obj(m.seasonCfg) ? m.seasonCfg : {})) {
      if (!obj(c)) continue;
      if (obj(c.al) && pos(c.al.id)) AL.add(+c.al.id);
      if (obj(c.bgm) && pos(c.bgm.id)) BGM.add(+c.bgm.id);
      if (obj(c.tmdb) && pos(c.tmdb.id)) TV.add(+c.tmdb.id);
    }
  }
  const locks = store.user && obj(store.user.seasonLocks) ? store.user.seasonLocks : {};
  for (const one of Object.values(locks)) for (const l of Object.values(obj(one) ? one : {})) {
    const pin = obj(l) && obj(l.pin) ? l.pin : null;
    if (!pin) continue;
    if (obj(pin.al) && pos(pin.al.id)) AL.add(+pin.al.id);
    if (obj(pin.bgm) && pos(pin.bgm.id)) BGM.add(+pin.bgm.id);
    if (obj(pin.tmdb) && pos(pin.tmdb.id)) TV.add(+pin.tmdb.id);
  }
  const media = obj(store.media) ? store.media : {};
  for (const id of AL) { const md = media[id]; if (obj(md) && pos(md.idMal)) MAL.add(+md.idMal); }

  // the dataset entries, as the app reads them
  const TMDB_RE = /^(tv|movie)\/(\d+)(?:-[^/]*)?(?:\/season\/(\d+)(?:\/episode\/(\d+))?)?$/;
  const ids = (it, site) => (it.sites || []).filter((s) => s && s.site === site && s.id != null && /^\d+$/.test(String(s.id))).map((s) => +s.id);
  const entries = DS.items.map((it, i) => {
    const tmdb = [];
    for (const s of it.sites || []) {
      if (!s || s.site !== 'tmdb' || s.id == null) continue;
      const m = TMDB_RE.exec(String(s.id));
      if (!m) continue;
      const t = { type: m[1], id: +m[2] };
      if (m[3] != null) t.season = +m[3];
      if (m[4] != null) t.ep = +m[4];
      tmdb.push(t);
    }
    return { i, title: String(it.title || ''), type: String(it.type || ''), begin: String(it.begin || '').slice(0, 10),
      anilist: [...new Set(ids(it, 'aniList'))], mal: [...new Set(ids(it, 'mal'))], bangumi: [...new Set(ids(it, 'bangumi'))], tmdb };
  });
  const keep = new Set();
  for (const e of entries) {
    if (e.anilist.some((x) => AL.has(x)) || e.mal.some((x) => MAL.has(x)) || e.bangumi.some((x) => BGM.has(x)) ||
      e.tmdb.some((t) => (t.type === 'movie' ? MOVIE : TV).has(t.id))) keep.add(e.i);
  }
  const series = new Set(TV);
  for (const i of keep) for (const t of entries[i].tmdb) if (t.type === 'tv') series.add(t.id);
  for (const e of entries) if (e.tmdb.some((t) => t.type === 'tv' && series.has(t.id))) keep.add(e.i);

  const lines = [];
  for (const e of entries) {
    if (!keep.has(e.i)) continue;
    const sites = {};
    if (e.anilist.length) sites.anilist = e.anilist;
    if (e.mal.length) sites.mal = e.mal;
    if (e.bangumi.length) sites.bangumi = e.bangumi;
    if (e.tmdb.length) sites.tmdb = e.tmdb;
    lines.push(JSON.stringify({ i: e.i, title: e.title, type: e.type, begin: e.begin, sites }));
  }
  const at = opt.at ? new Date(opt.at) : new Date(snap || 0);
  if (Number.isNaN(at.getTime())) throw Object.assign(new Error('--at is not a time'), { refused: true });
  const head = { src: 'bangumi-data@' + opt.ver, commit: opt.commit, sha256: opt.sha256, license: 'CC-BY-4.0', source: 'https://github.com/bangumi-data/bangumi-data',
    generatedAt: at.toISOString(), n: lines.length };
  const text = '{\n' + Object.entries(head).map(([k, v]) => JSON.stringify(k) + ': ' + JSON.stringify(v) + ',\n').join('') +
    '"entries": [\n' + lines.join(',\n') + '\n]\n}\n';
  return { text, stats: { entries: lines.length, bytes: Buffer.byteLength(text), libraryIds: { al: AL.size, mal: MAL.size, bgm: BGM.size, movie: MOVIE.size, tv: TV.size }, series: series.size } };
}

if (require.main === module) {
  zh(['============================================================', ' 影片播放器 bangumi-data 索引（只含本機媒體庫相關條目）', '============================================================',
    ' 資料集：' + OPT.dataset, ' 比對資料（只讀副本）：' + OPT.store, ' 輸出：' + OPT.out].join('\n'));
  if (!OPT.dataset) refuse('--dataset <data.json> is required');
  if (!OPT.store) refuse('--store <anilist.json copy> is required');
  if (!OPT.live && /[\\/]data[\\/]anilist\.json$/i.test(path.resolve(OPT.store))) refuse('that looks like the live data folder; pass a copy, or --live to read it in place (read only)');
  let dsBuf, storeText;
  try { dsBuf = fs.readFileSync(OPT.dataset); } catch (e) { refuse('cannot read the dataset: ' + e.message, '找不到或無法讀取資料集檔案（請放 bangumi-data v' + PIN.ver + ' 的 dist\data.json）'); }
  try { storeText = fs.readFileSync(OPT.store, 'utf8'); } catch (e) { refuse('cannot read the store: ' + e.message, '無法讀取比對資料'); }
  let r;
  try { r = build(dsBuf, storeText, OPT); } catch (e) { if (e.refused) refuse(e.message, /SHA-256/.test(e.message) ? '資料集檔案不是釘選的 bangumi-data v' + PIN.ver + '（SHA-256 不符）' : '檔案格式不符（' + e.message + '）'); throw e; }
  const outSha = sha256(Buffer.from(r.text));
  if (OPT.check) {
    let old = null;
    try { old = fs.readFileSync(OPT.out, 'utf8'); } catch (e) { old = null; }
    const same = old != null && old.replace(/\r\n/g, '\n') === r.text; // a checkout may have CRLF line ends (core.autocrlf)
    console.log('[bangumi-data-index] ' + (same ? 'identical' : 'DIFFERENT') + ': ' + OPT.out + ' (' + r.stats.entries + ' entries, ' + r.stats.bytes + ' bytes, sha256 ' + outSha + ')');
    process.exit(same ? 0 : 1);
  }
  fs.mkdirSync(path.dirname(OPT.out), { recursive: true });
  fs.writeFileSync(OPT.out + '.tmp', r.text);
  fs.renameSync(OPT.out + '.tmp', OPT.out);
  console.log('[bangumi-data-index] wrote ' + OPT.out);
  console.log('  entries ' + r.stats.entries + ', bytes ' + r.stats.bytes + ', sha256 ' + outSha);
  console.log('  library ids: ' + JSON.stringify(r.stats.libraryIds) + ', tmdb series kept whole: ' + r.stats.series);
  zh('[完成] 索引已更新（' + r.stats.entries + ' 個條目）。請重新啟動播放器；新作品的資料集建議會出現在「設定 → 線上資料 → 待複查」。');
}

module.exports = { build, PIN };
