'use strict';
const { contextBridge, ipcRenderer } = require('electron');
contextBridge.exposeInMainWorld('find', {
  query: (text, forward) => ipcRenderer.send('find:query', { text, forward }),
  close: () => ipcRenderer.send('find:close'),
  onResult: (fn) => ipcRenderer.on('find:result', (_e, r) => fn(r)),
  onShow: (fn) => ipcRenderer.on('find:show', () => fn()),
});
