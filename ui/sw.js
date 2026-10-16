// Pass-through service worker: exists only so Chrome treats the app as installable.
// It never caches anything, so a new host build is always picked up immediately.
self.addEventListener('install', () => self.skipWaiting());
self.addEventListener('activate', (e) => e.waitUntil(self.clients.claim()));
self.addEventListener('fetch', () => {});
