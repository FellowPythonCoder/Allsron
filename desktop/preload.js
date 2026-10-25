// Sreon Desktop — preload.js
// Runs in every page. Sreon's own pages (sreon://) get the same bridge the Mac app has,
// so the interface code does not change. Websites get the Shield script, and nothing else.
'use strict';
const { contextBridge, ipcRenderer, webFrame } = require('electron');

if (location.protocol === 'sreon:') {
  contextBridge.exposeInMainWorld('webkit', {
    messageHandlers: {
      sreon: { postMessage: (message) => ipcRenderer.invoke('sreon:msg', message) },
    },
  });
  window.addEventListener('DOMContentLoaded', () => {
    document.documentElement.setAttribute('data-platform', process.platform);
  });
} else {
  try {
    const source = ipcRenderer.sendSync('sreon:web-source');
    if (source) webFrame.executeJavaScript(source);
  } catch (e) { /* a page must still load if the script cannot */ }
}
