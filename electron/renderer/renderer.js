const RESTING_GIFS = [
  "../../public/pets/v2/resting_candidate_sleep.gif",
];

const GIF_BY_STATE = {
  1: "../../public/pets/v2/walking_candidate.gif",
  2: "../../public/pets/v2/impact_candidate.gif",
  3: "../../public/pets/v2/vigorous_activity.gif",
  4: "../../public/pets/v2/other_activity.gif",
  255: RESTING_GIFS[0],
};

const STATE_NAME = {
  0: "静息",
  1: "行走",
  2: "碰撞",
  3: "剧烈活动",
  4: "其他活动",
  255: "预热中",
};

const STORAGE_KEY = "cat-live-pet.desktop.config.v1";
const RESTING_VARIANT_INTERVAL_MS = 12000;
const root = document.querySelector("#petRoot");
const petButton = document.querySelector("#petButton");
const petGif = document.querySelector("#petGif");
const batteryStatus = document.querySelector("#batteryStatus");
const connectionNotice = document.querySelector("#connectionNotice");
const connectionMessage = document.querySelector("#connectionMessage");
const openConnectionSettings = document.querySelector("#openConnectionSettings");
const retryConnection = document.querySelector("#retryConnection");
const liveStatus = document.querySelector("#liveStatus");
const settings = document.querySelector("#settings");
const settingsForm = document.querySelector("#settingsForm");
const webSocketUrlInput = document.querySelector("#webSocketUrl");
const deviceIdInput = document.querySelector("#deviceId");
const configError = document.querySelector("#configError");
const cancelSettings = document.querySelector("#cancelSettings");
const closeWindow = document.querySelector("#closeWindow");
const petSizeInput = document.querySelector("#petSize");
const petSizeValue = document.querySelector("#petSizeValue");

let config = null;
let socket = null;
let reconnectTimer = null;
let heartbeatTimer = null;
let reconnectDelay = 1000;
let stopped = false;
let currentState = null;
let restingVariantIndex = 0;
let restingVariantTimer = null;
let dragPointerId = null;
let dragOrigin = null;
let pressStartedAt = 0;
let isWindowDragging = false;
let suppressDoubleClickUntil = 0;

const DRAG_THRESHOLD = 4;
const LONG_PRESS_THRESHOLD = 300;

function normalizeSocketUrl(rawValue) {
  const trimmed = rawValue.trim();
  if (!trimmed) throw new Error("请输入 WebSocket 地址");
  const withScheme = trimmed.includes("://") ? trimmed : `ws://${trimmed}`;
  const url = new URL(withScheme);

  if (url.protocol === "http:") url.protocol = "ws:";
  if (url.protocol === "https:") url.protocol = "wss:";
  if (url.protocol !== "ws:" && url.protocol !== "wss:") {
    throw new Error("地址必须使用 ws:// 或 wss://");
  }
  if (url.pathname === "/") url.pathname = "/ws";
  return url.toString();
}

function viewerSocketUrl(value) {
  const url = new URL(value.webSocketUrl);
  url.searchParams.set("device", value.deviceId);
  url.searchParams.set("role", "viewer");
  return url.toString();
}

function showSettings() {
  window.catDesktop?.setSettingsOpen(true);
  webSocketUrlInput.value = config?.webSocketUrl || "";
  deviceIdInput.value = config?.deviceId || "cat-01";
  cancelSettings.hidden = !config;
  configError.textContent = "";
  connectionNotice.hidden = true;
  settings.hidden = false;
  webSocketUrlInput.focus();
  window.catDesktop?.getPetSize().then((size) => {
    petSizeInput.value = String(size);
    petSizeValue.value = `${size} px`;
  });
}

function showSizeSettings() {
  showSettings();
  petSizeInput.focus();
}

function hideSettings() {
  if (!config) return;
  settings.hidden = true;
  window.catDesktop?.setSettingsOpen(false);
  connectionNotice.hidden = root.dataset.connected === "true";
}

function setConnectionStatus(connected, message = "") {
  root.dataset.connected = String(connected);
  connectionMessage.textContent = message;
  connectionNotice.hidden = connected || !settings.hidden;
}

function clearConnectionTimers() {
  clearTimeout(reconnectTimer);
  clearInterval(heartbeatTimer);
  reconnectTimer = null;
  heartbeatTimer = null;
}

function closeSocket() {
  stopRestingRotation();
  currentState = null;
  setBatteryPercent(null);
  clearConnectionTimers();
  if (socket) {
    socket.onclose = null;
    socket.close();
    socket = null;
  }
  setConnectionStatus(false, "连接已断开");
}

function setBatteryPercent(value) {
  if (!Number.isInteger(value) || value < 0 || value > 100) {
    batteryStatus.hidden = true;
    batteryStatus.removeAttribute("data-level");
    return;
  }

  batteryStatus.hidden = false;
  batteryStatus.textContent = `电量 ${value}%`;
  batteryStatus.title = `XIAO 电池剩余电量约 ${value}%`;
  batteryStatus.dataset.level = value <= 10
    ? "critical"
    : value <= 25
      ? "low"
      : "normal";
}

function setState(state) {
  if (state === 0) {
    const enteredResting = currentState !== state;
    currentState = state;
    if (enteredResting) {
      restingVariantIndex = 0;
      showRestingVariant();
    }
    return;
  }

  if (!Object.hasOwn(GIF_BY_STATE, state)) return;
  stopRestingRotation();
  currentState = state;
  setPetSource(GIF_BY_STATE[state]);
  petGif.alt = `${STATE_NAME[state] || "猫咪"}动画`;
  liveStatus.textContent = STATE_NAME[state] || "未知状态";
}

function setPetSource(source) {
  if (petGif.getAttribute("src") !== source) petGif.src = source;
}

function stopRestingRotation() {
  clearTimeout(restingVariantTimer);
  restingVariantTimer = null;
}

function showRestingVariant() {
  stopRestingRotation();
  setPetSource(RESTING_GIFS[restingVariantIndex]);
  petGif.alt = `${STATE_NAME[0]}动画`;
  liveStatus.textContent = STATE_NAME[0];
  restingVariantTimer = setTimeout(() => {
    if (currentState !== 0) return;
    restingVariantIndex = (restingVariantIndex + 1) % RESTING_GIFS.length;
    showRestingVariant();
  }, RESTING_VARIANT_INTERVAL_MS);
}

function connect() {
  closeSocket();
  if (!config) return;
  stopped = false;
  setConnectionStatus(false, "正在连接…");

  try {
    socket = new WebSocket(viewerSocketUrl(config));
  } catch {
    showSettings();
    return;
  }

  socket.onopen = () => {
    reconnectDelay = 1000;
    setConnectionStatus(true);
    heartbeatTimer = setInterval(() => {
      if (socket?.readyState === WebSocket.OPEN) socket.send("ping");
    }, 20000);
  };

  socket.onmessage = (event) => {
    if (event.data === "pong") return;
    try {
      const message = JSON.parse(String(event.data));
      if (
        message.type === "state" &&
        message.protocol === 1 &&
        message.deviceId === config.deviceId &&
        Number.isInteger(message.state)
      ) {
        setBatteryPercent(message.batteryPercent);
        setState(message.state);
      }
    } catch {
      // 忽略非协议消息。
    }
  };

  socket.onerror = () => socket?.close();
  socket.onclose = () => {
    setConnectionStatus(false, "连接已断开，正在重试…");
    clearInterval(heartbeatTimer);
    heartbeatTimer = null;
    if (stopped) return;
    reconnectTimer = setTimeout(connect, reconnectDelay);
    reconnectDelay = Math.min(reconnectDelay * 2, 15000);
  };
}

function loadConfig() {
  try {
    const saved = localStorage.getItem(STORAGE_KEY);
    if (saved) config = JSON.parse(saved);
  } catch {
    localStorage.removeItem(STORAGE_KEY);
  }

  if (config?.webSocketUrl && config?.deviceId) {
    settings.hidden = true;
    connect();
  } else {
    config = null;
    showSettings();
  }
}

function screenPoint(event) {
  return { x: event.screenX, y: event.screenY };
}

function finishPetDrag(event) {
  if (dragPointerId === null || (event && event.pointerId !== dragPointerId)) return;

  const pointerId = dragPointerId;
  const wasLongPress = performance.now() - pressStartedAt >= LONG_PRESS_THRESHOLD;
  dragPointerId = null;
  dragOrigin = null;
  pressStartedAt = 0;
  petButton.classList.remove("is-dragging");

  if (isWindowDragging || wasLongPress) {
    suppressDoubleClickUntil = performance.now() + 350;
  }
  if (isWindowDragging) {
    window.catDesktop?.endDrag();
  }
  isWindowDragging = false;

  if (petButton.hasPointerCapture(pointerId)) {
    petButton.releasePointerCapture(pointerId);
  }
}

petButton.addEventListener("pointerdown", (event) => {
  if (event.button !== 0 || !event.isPrimary) return;

  dragPointerId = event.pointerId;
  dragOrigin = screenPoint(event);
  pressStartedAt = performance.now();
  isWindowDragging = false;
  petButton.classList.add("is-dragging");
  petButton.setPointerCapture(event.pointerId);
});

petButton.addEventListener("pointermove", (event) => {
  if (event.pointerId !== dragPointerId || !dragOrigin) return;

  const point = screenPoint(event);
  if (!isWindowDragging) {
    const distance = Math.hypot(point.x - dragOrigin.x, point.y - dragOrigin.y);
    if (distance < DRAG_THRESHOLD) return;
    isWindowDragging = true;
    window.catDesktop?.startDrag(dragOrigin);
  }

  window.catDesktop?.moveDrag(point);
  event.preventDefault();
});

petButton.addEventListener("pointerup", finishPetDrag);
petButton.addEventListener("pointercancel", finishPetDrag);
petButton.addEventListener("lostpointercapture", finishPetDrag);

settingsForm.addEventListener("submit", (event) => {
  event.preventDefault();
  configError.textContent = "";

  try {
    const nextConfig = {
      webSocketUrl: normalizeSocketUrl(webSocketUrlInput.value),
      deviceId: deviceIdInput.value.trim(),
    };
    if (!/^[A-Za-z0-9_-]{1,64}$/.test(nextConfig.deviceId)) {
      throw new Error("设备编号只能使用字母、数字、下划线和短横线");
    }

    config = nextConfig;
    localStorage.setItem(STORAGE_KEY, JSON.stringify(config));
    window.catDesktop?.setPetSize(Number(petSizeInput.value));
    hideSettings();
    connect();
  } catch (error) {
    configError.textContent = error instanceof Error ? error.message : "配置无效";
  }
});

cancelSettings.addEventListener("click", hideSettings);
closeWindow.addEventListener("click", () => window.catDesktop?.quit());
petSizeInput.addEventListener("input", () => {
  petSizeValue.value = `${petSizeInput.value} px`;
});
openConnectionSettings.addEventListener("click", showSettings);
retryConnection.addEventListener("click", connect);
petButton.addEventListener("dblclick", () => {
  if (performance.now() >= suppressDoubleClickUntil) showSettings();
});
window.catDesktop?.onOpenSettings(showSettings);
window.catDesktop?.onOpenSizeSettings(showSizeSettings);
window.catDesktop?.onReconnect(connect);
window.addEventListener("beforeunload", () => {
  finishPetDrag();
  stopped = true;
  closeSocket();
});

loadConfig();
