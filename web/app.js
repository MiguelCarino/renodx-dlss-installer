// SPDX-License-Identifier: GPL-3.0-only
// dlss.carino.systems. Two buttons: the AppImage (any recent x86-64 Linux) and
// Fedora's RPM. Each href downloads its file, the no-JS path; a click also opens
// the sheet that says what to do next. Firefox on Fedora names Fedora in its user
// agent, so Fedora readers see the RPM marked and other Linux readers the
// AppImage. The links follow the latest GitHub release when the API answers.
'use strict';

const REPO = 'MiguelCarino/renodx-dlss-installer';
const APPIMAGE = /^DLSS5_for_Linux-.*-x86_64\.AppImage$/;
const RPM = /^dlss5-linux-.*\.noarch\.rpm$/;

const $ = (id) => document.getElementById(id);
const chips = Array.from(document.querySelectorAll('#downloads .dl-btn'));
const dialogs = { fedora: $('fedoraDialog'), appimage: $('appimageDialog') };

// ---------------------------------------------------------------- which system is this?
function detect() {
  const ua = navigator.userAgent || '';
  const p = ((navigator.userAgentData && navigator.userAgentData.platform) || navigator.platform || '').toLowerCase();
  if (/android/i.test(ua)) return null;                 // a phone says Linux too
  if (/fedora/i.test(ua)) return 'fedora';
  if (p.includes('linux') || /x11|linux/i.test(ua)) return 'appimage';
  return null;
}
const os = detect();
for (const c of chips) if (c.dataset.kind === os) c.classList.add('recommended');
if (os) $('osNote').hidden = true;

// ---------------------------------------------------------------- the sheets
chips.forEach((c) => c.addEventListener('click', () => dialogs[c.dataset.kind].showModal()));
$('installBtn').addEventListener('click', () => dialogs.fedora.showModal());
for (const d of Object.values(dialogs)) {
  d.addEventListener('click', (e) => { if (e.target === d) d.close(); });   // backdrop closes
}

// ---------------------------------------------------------------- copy
async function copyFrom(button) {
  const text = $(button.dataset.src).textContent;
  try {
    await navigator.clipboard.writeText(text);
  } catch {
    const ta = document.createElement('textarea');
    ta.value = text;
    document.body.append(ta);
    ta.select();
    document.execCommand('copy');
    ta.remove();
  }
  button.textContent = 'Copied';
  button.classList.add('done');
  setTimeout(() => { button.textContent = 'Copy'; button.classList.remove('done'); }, 1600);
}
document.querySelectorAll('.copy').forEach((b) => b.addEventListener('click', () => copyFrom(b)));

// ---------------------------------------------------------------- latest release
function apply(tag, assets) {
  const v = tag.replace(/^v/, '');
  const find = (re) => assets.find((a) => re.test(a.name));
  const app = find(APPIMAGE), rpm = find(RPM), sums = find(/^SHA256SUMS$/);
  document.querySelector('#version strong').textContent = v;
  $('foot-version').textContent = `DLSS 5 for Linux ${v}`;
  for (const c of chips) {
    const a = c.dataset.kind === 'fedora' ? rpm : app;
    if (a) c.href = a.url;
  }
  if (rpm) $('cmd').textContent = `sudo dnf install ${rpm.url}`;
  if (app) $('appcmd').textContent = `chmod +x ${app.name}\n./${app.name}`;
  const page = `https://github.com/${REPO}/releases/tag/${tag}`;
  document.querySelectorAll('.sums').forEach((s) => { s.href = sums ? sums.url : page; });
}

fetch(`https://api.github.com/repos/${REPO}/releases/latest`, { headers: { Accept: 'application/vnd.github+json' } })
  .then((r) => (r.ok ? r.json() : Promise.reject(r.status)))
  .then((data) => {
    if (!data || !Array.isArray(data.assets) || !data.assets.length) return;
    apply(data.tag_name, data.assets.map((a) => ({ name: a.name, url: a.browser_download_url })));
  })
  .catch(() => { /* offline or rate-limited: the built-in 0.0.6 links stay */ });

// ---------------------------------------------------------------- one screen, no scroll
// The page is one screen. Where it is taller than the window (a short laptop
// screen, a phone), the hero is zoomed down just enough to fit, never below 60%.
function fitScreen() {
  const wrap = document.querySelector('.hero > .wrap');
  wrap.style.zoom = '';
  const fits = () => document.documentElement.scrollHeight <= window.innerHeight;
  if (fits()) return;
  // Text rewraps as it shrinks, so the height is not linear in the zoom: search for
  // the largest zoom that fits rather than computing one.
  let lo = 0.6, hi = 1;
  for (let k = 0; k < 7; k++) {
    const mid = (lo + hi) / 2;
    wrap.style.zoom = String(mid);
    if (fits()) lo = mid; else hi = mid;
  }
  wrap.style.zoom = String(lo);
}
fitScreen();
window.addEventListener('resize', fitScreen);
if (document.fonts) document.fonts.ready.then(fitScreen);
