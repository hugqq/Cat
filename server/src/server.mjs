import { createServer } from "node:http";
import { WebSocket, WebSocketServer } from "ws";

const port = Number.parseInt(process.env.PORT || "8787", 10);
const publishToken = process.env.CAT_PUBLISH_TOKEN || "";

if (!publishToken) {
  console.error("CAT_PUBLISH_TOKEN is required");
  process.exit(1);
}

const stateNames = {
  0: "resting_candidate",
  1: "walking_candidate",
  2: "impact_candidate",
  3: "vigorous_activity",
  4: "other_activity",
  255: "warming_up",
};

const rooms = new Map();

function getRoom(deviceId) {
  let room = rooms.get(deviceId);
  if (!room) {
    room = { latest: null, viewers: new Set(), devices: new Set() };
    rooms.set(deviceId, room);
  }
  return room;
}

function json(response, status, value) {
  response.writeHead(status, {
    "content-type": "application/json; charset=utf-8",
    "cache-control": "no-store",
  });
  response.end(JSON.stringify(value));
}

const server = createServer((request, response) => {
  const url = new URL(request.url || "/", "http://localhost");
  if (url.pathname === "/health") {
    json(response, 200, {
      ok: true,
      service: "cat-state-websocket-server",
      protocol: 1,
      rooms: rooms.size,
    });
    return;
  }
  json(response, 404, {
    error: "not_found",
    websocket: "/ws?device=cat-01&role=viewer",
  });
});

const webSocketServer = new WebSocketServer({ noServer: true, maxPayload: 1024 });

function rejectUpgrade(socket, status, message) {
  const body = JSON.stringify({ error: message });
  socket.write(
    `HTTP/1.1 ${status}\r\nContent-Type: application/json\r\nContent-Length: ${Buffer.byteLength(body)}\r\nConnection: close\r\n\r\n${body}`,
  );
  socket.destroy();
}

server.on("upgrade", (request, socket, head) => {
  const url = new URL(request.url || "/", "http://localhost");
  if (url.pathname !== "/ws") {
    rejectUpgrade(socket, "404 Not Found", "not_found");
    return;
  }

  const deviceId = url.searchParams.get("device") || "";
  const role = url.searchParams.get("role") || "";
  if (!/^[A-Za-z0-9_-]{1,64}$/.test(deviceId)) {
    rejectUpgrade(socket, "400 Bad Request", "invalid_device_id");
    return;
  }
  if (role !== "device" && role !== "viewer") {
    rejectUpgrade(socket, "400 Bad Request", "invalid_role");
    return;
  }

  if (role === "device") {
    const authorization = request.headers.authorization || "";
    const bearer = authorization.startsWith("Bearer ")
      ? authorization.slice("Bearer ".length).trim()
      : "";
    const suppliedToken = bearer || url.searchParams.get("token") || "";
    if (suppliedToken !== publishToken) {
      rejectUpgrade(socket, "401 Unauthorized", "unauthorized");
      return;
    }
  }

  webSocketServer.handleUpgrade(request, socket, head, (webSocket) => {
    webSocketServer.emit("connection", webSocket, request, { deviceId, role });
  });
});

webSocketServer.on("connection", (socket, _request, connection) => {
  const { deviceId, role } = connection;
  const room = getRoom(deviceId);
  socket.isAlive = true;
  socket.on("pong", () => {
    socket.isAlive = true;
  });

  if (role === "viewer") {
    room.viewers.add(socket);
    socket.send(
      JSON.stringify(
        room.latest || { type: "waiting", protocol: 1, deviceId },
      ),
    );
  } else {
    room.devices.add(socket);
    socket.send(JSON.stringify({ type: "ready", protocol: 1, deviceId }));
  }

  socket.on("message", (data, isBinary) => {
    if (isBinary) return;
    const raw = data.toString();
    if (raw === "ping") {
      socket.send("pong");
      return;
    }
    if (role !== "device") {
      socket.send(JSON.stringify({ type: "error", error: "viewer_is_read_only" }));
      return;
    }

    let incoming;
    try {
      incoming = JSON.parse(raw);
    } catch {
      socket.send(JSON.stringify({ type: "error", error: "invalid_json" }));
      return;
    }

    const state = incoming.state;
    if (
      incoming.type !== "state" ||
      incoming.protocol !== 1 ||
      !Number.isInteger(state) ||
      !Object.hasOwn(stateNames, state)
    ) {
      socket.send(JSON.stringify({ type: "error", error: "invalid_state" }));
      return;
    }

    const batteryPercent =
      Number.isInteger(incoming.batteryPercent) &&
      incoming.batteryPercent >= 0 &&
      incoming.batteryPercent <= 100
        ? incoming.batteryPercent
        : null;

    const message = {
      type: "state",
      protocol: 1,
      deviceId,
      state,
      name: stateNames[state],
      batteryPercent,
      seq: Number.isInteger(incoming.seq) ? incoming.seq : null,
      sourceMs: Number.isInteger(incoming.sourceMs) ? incoming.sourceMs : null,
      updatedAt: new Date().toISOString(),
    };
    room.latest = message;
    const encoded = JSON.stringify(message);

    for (const viewer of room.viewers) {
      if (viewer.readyState === WebSocket.OPEN) viewer.send(encoded);
    }
    socket.send(JSON.stringify({ type: "ack", protocol: 1, state, seq: message.seq }));
  });

  socket.on("close", () => {
    room.viewers.delete(socket);
    room.devices.delete(socket);
  });
});

const heartbeat = setInterval(() => {
  for (const socket of webSocketServer.clients) {
    if (!socket.isAlive) {
      socket.terminate();
      continue;
    }
    socket.isAlive = false;
    socket.ping();
  }
}, 30000);

server.listen(port, "0.0.0.0", () => {
  console.log(`Cat WebSocket server listening on 0.0.0.0:${port}`);
});

function shutdown() {
  clearInterval(heartbeat);
  for (const socket of webSocketServer.clients) socket.close(1001, "server_shutdown");
  server.close(() => process.exit(0));
  setTimeout(() => process.exit(1), 5000).unref();
}

process.on("SIGINT", shutdown);
process.on("SIGTERM", shutdown);
