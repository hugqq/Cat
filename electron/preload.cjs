const { contextBridge, ipcRenderer } = require("electron");

function on(channel, callback) {
  const listener = () => callback();
  ipcRenderer.on(channel, listener);
  return () => ipcRenderer.removeListener(channel, listener);
}

contextBridge.exposeInMainWorld("catDesktop", {
  onOpenSettings: (callback) => on("cat:open-settings", callback),
  onOpenSizeSettings: (callback) => on("cat:open-size-settings", callback),
  onReconnect: (callback) => on("cat:reconnect", callback),
  startDrag: (point) => ipcRenderer.send("cat:drag-start", point),
  moveDrag: (point) => ipcRenderer.send("cat:drag-move", point),
  endDrag: () => ipcRenderer.send("cat:drag-end"),
  setSettingsOpen: (open) => ipcRenderer.send("cat:settings-mode", open),
  getPetSize: () => ipcRenderer.invoke("cat:get-pet-size"),
  setPetSize: (size) => ipcRenderer.send("cat:set-pet-size", size),
  quit: () => ipcRenderer.send("cat:quit"),
});
