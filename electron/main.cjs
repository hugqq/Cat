const { app, BrowserWindow, ipcMain, Menu, Tray, nativeImage, screen } = require("electron");
const fs = require("node:fs");
const path = require("node:path");

let mainWindow = null;
let tray = null;
let alwaysOnTop = true;
let saveBoundsTimer = null;
let dragSession = null;
let petBounds = null;
let settingsMode = false;

const BOUNDS_VERSION = 4;
const DEFAULT_PET_SIZE = 240;
const MIN_PET_SIZE = 160;
const MAX_PET_SIZE = 420;
const SETTINGS_WIDTH = 480;
const SETTINGS_HEIGHT = 520;

const gotSingleInstanceLock = app.requestSingleInstanceLock();
if (!gotSingleInstanceLock) app.quit();

function boundsFile() {
  return path.join(app.getPath("userData"), "window-bounds.json");
}

function loadBounds() {
  try {
    const value = JSON.parse(fs.readFileSync(boundsFile(), "utf8"));
    if (
      Number.isInteger(value.x) &&
      Number.isInteger(value.y) &&
      Number.isInteger(value.width) &&
      Number.isInteger(value.height)
    ) {
      const size =
        value.width === value.height &&
        value.width >= MIN_PET_SIZE &&
        value.width <= MAX_PET_SIZE
          ? value.width
          : DEFAULT_PET_SIZE;
      if (
        value.version !== BOUNDS_VERSION ||
        value.width !== size ||
        value.height !== size
      ) {
        return {
          x: Math.round(value.x + (value.width - size) / 2),
          y: Math.round(value.y + (value.height - size) / 2),
          width: size,
          height: size,
        };
      }
      return value;
    }
  } catch {
    // 第一次启动时没有位置记录。
  }
  return { width: DEFAULT_PET_SIZE, height: DEFAULT_PET_SIZE };
}

function saveBounds() {
  if (!mainWindow || mainWindow.isDestroyed() || settingsMode) return;
  clearTimeout(saveBoundsTimer);
  saveBoundsTimer = setTimeout(() => {
    try {
      const currentBounds = mainWindow.getBounds();
      petBounds = {
        x: currentBounds.x,
        y: currentBounds.y,
        width: petBounds.width,
        height: petBounds.height,
      };
      fs.writeFileSync(
        boundsFile(),
        JSON.stringify({ ...petBounds, version: BOUNDS_VERSION }),
      );
    } catch {
      // 保存失败不影响桌面宠物继续运行。
    }
  }, 200);
}

function showWindow() {
  if (!mainWindow || mainWindow.isDestroyed()) return;
  if (mainWindow.isMinimized()) mainWindow.restore();
  mainWindow.showInactive();
}

function applyAlwaysOnTop() {
  if (!mainWindow || mainWindow.isDestroyed()) return;
  mainWindow.setAlwaysOnTop(!settingsMode && alwaysOnTop, "normal");
}

function sendToRenderer(channel) {
  if (!mainWindow || mainWindow.isDestroyed()) return;
  showWindow();
  mainWindow.webContents.send(channel);
}

function setPetSize(size) {
  if (!mainWindow || mainWindow.isDestroyed()) return;
  size = Math.max(MIN_PET_SIZE, Math.min(MAX_PET_SIZE, Math.round(size)));
  if (settingsMode) {
    petBounds = { ...petBounds, width: size, height: size };
    return;
  }
  const currentBounds = mainWindow.getBounds();
  petBounds = { ...currentBounds, width: size, height: size };
  mainWindow.setSize(size, size, true);
  saveBounds();
}

function centeredBounds(bounds, width, height) {
  const display = screen.getDisplayMatching(bounds);
  const area = display.workArea;
  const x = Math.max(
    area.x,
    Math.min(Math.round(bounds.x + (bounds.width - width) / 2), area.x + area.width - width),
  );
  const y = Math.max(
    area.y,
    Math.min(Math.round(bounds.y + (bounds.height - height) / 2), area.y + area.height - height),
  );
  return { x, y, width, height };
}

function setSettingsMode(open) {
  if (!mainWindow || mainWindow.isDestroyed() || settingsMode === open) return;

  if (open) {
    clearTimeout(saveBoundsTimer);
    const currentBounds = mainWindow.getBounds();
    petBounds = {
      x: currentBounds.x,
      y: currentBounds.y,
      width: petBounds.width,
      height: petBounds.height,
    };
    settingsMode = true;
    applyAlwaysOnTop();
    mainWindow.setFocusable(true);
    mainWindow.setBounds(centeredBounds(petBounds, SETTINGS_WIDTH, SETTINGS_HEIGHT), true);
    mainWindow.show();
    mainWindow.focus();
    return;
  }

  const currentBounds = mainWindow.getBounds();
  const nextBounds = centeredBounds(currentBounds, petBounds.width, petBounds.height);
  mainWindow.setBounds(nextBounds, true);
  petBounds = nextBounds;
  settingsMode = false;
  mainWindow.setFocusable(false);
  mainWindow.blur();
  applyAlwaysOnTop();
  mainWindow.showInactive();
  saveBounds();
}

function isValidPoint(point) {
  return Number.isFinite(point?.x) && Number.isFinite(point?.y);
}

ipcMain.on("cat:drag-start", (event, point) => {
  if (event.sender !== mainWindow?.webContents || !isValidPoint(point)) return;
  const currentBounds = mainWindow.getBounds();
  dragSession = {
    senderId: event.sender.id,
    pointer: point,
    bounds: {
      x: currentBounds.x,
      y: currentBounds.y,
      width: petBounds.width,
      height: petBounds.height,
    },
  };
});

ipcMain.on("cat:drag-move", (event, point) => {
  if (
    !dragSession ||
    event.sender.id !== dragSession.senderId ||
    !isValidPoint(point) ||
    !mainWindow ||
    mainWindow.isDestroyed()
  ) {
    return;
  }

  mainWindow.setBounds({
    x: Math.round(dragSession.bounds.x + point.x - dragSession.pointer.x),
    y: Math.round(dragSession.bounds.y + point.y - dragSession.pointer.y),
    width: dragSession.bounds.width,
    height: dragSession.bounds.height,
  });
});

ipcMain.on("cat:drag-end", (event) => {
  if (event.sender.id !== dragSession?.senderId) return;
  dragSession = null;
  saveBounds();
});

ipcMain.on("cat:settings-mode", (event, open) => {
  if (event.sender !== mainWindow?.webContents || typeof open !== "boolean") return;
  setSettingsMode(open);
});

ipcMain.on("cat:set-pet-size", (event, size) => {
  if (event.sender !== mainWindow?.webContents || !Number.isFinite(size)) return;
  setPetSize(size);
});

ipcMain.handle("cat:get-pet-size", (event) => {
  if (event.sender !== mainWindow?.webContents) return DEFAULT_PET_SIZE;
  return petBounds?.width || DEFAULT_PET_SIZE;
});

ipcMain.on("cat:quit", (event) => {
  if (event.sender !== mainWindow?.webContents) return;
  app.quit();
});

function menuTemplate() {
  return [
    {
      label: "连接设置",
      click: () => sendToRenderer("cat:open-settings"),
    },
    {
      label: "重新连接",
      click: () => sendToRenderer("cat:reconnect"),
    },
    { type: "separator" },
    {
      label: "始终置顶",
      type: "checkbox",
      checked: alwaysOnTop,
      click: (item) => {
        alwaysOnTop = item.checked;
        applyAlwaysOnTop();
        if (tray) tray.setContextMenu(Menu.buildFromTemplate(menuTemplate()));
      },
    },
    { label: "宠物大小…", click: () => sendToRenderer("cat:open-size-settings") },
    { type: "separator" },
    { label: "退出", click: () => app.quit() },
  ];
}

function popupMenu() {
  Menu.buildFromTemplate(menuTemplate()).popup({ window: mainWindow });
}

function createTray() {
  const candidates = [
    path.join(__dirname, "..", "public", "favicon.svg"),
    path.join(__dirname, "..", "public", "pets", "v2", "resting_candidate_sleep.gif"),
  ];

  let icon = nativeImage.createEmpty();
  for (const candidate of candidates) {
    icon = nativeImage.createFromPath(candidate);
    if (!icon.isEmpty()) break;
  }
  if (icon.isEmpty()) return;

  tray = new Tray(icon.resize({ width: 16, height: 16 }));
  tray.setToolTip("猫咪桌面宠物");
  tray.setContextMenu(Menu.buildFromTemplate(menuTemplate()));
  tray.on("click", showWindow);
}

function createWindow() {
  const bounds = loadBounds();
  petBounds = bounds;
  settingsMode = false;
  mainWindow = new BrowserWindow({
    ...bounds,
    minWidth: 160,
    minHeight: 160,
    maxWidth: SETTINGS_WIDTH,
    maxHeight: SETTINGS_HEIGHT,
    resizable: false,
    maximizable: false,
    frame: false,
    transparent: true,
    backgroundColor: "#00000000",
    hasShadow: false,
    alwaysOnTop,
    focusable: false,
    skipTaskbar: true,
    autoHideMenuBar: true,
    show: false,
    webPreferences: {
      preload: path.join(__dirname, "preload.cjs"),
      contextIsolation: true,
      nodeIntegration: false,
      sandbox: true,
    },
  });

  applyAlwaysOnTop();
  mainWindow.loadFile(path.join(__dirname, "renderer", "index.html"));
  mainWindow.once("ready-to-show", () => mainWindow?.showInactive());
  mainWindow.on("move", saveBounds);
  mainWindow.on("resize", () => {
    if (settingsMode) return;
    const currentBounds = mainWindow.getBounds();
    if (
      currentBounds.width !== petBounds.width ||
      currentBounds.height !== petBounds.height
    ) {
      mainWindow.setBounds({
        x: currentBounds.x,
        y: currentBounds.y,
        width: petBounds.width,
        height: petBounds.height,
      });
      return;
    }
    saveBounds();
  });
  mainWindow.on("will-resize", (event) => event.preventDefault());
  mainWindow.webContents.on("context-menu", (_event, parameters) => {
    if (!parameters.isEditable) popupMenu();
  });
  mainWindow.on("closed", () => {
    dragSession = null;
    mainWindow = null;
  });
}

app.whenReady().then(() => {
  Menu.setApplicationMenu(null);
  createWindow();
  createTray();
});

app.on("second-instance", showWindow);
app.on("activate", () => {
  if (BrowserWindow.getAllWindows().length === 0) createWindow();
  else showWindow();
});
app.on("window-all-closed", () => app.quit());
