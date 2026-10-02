import { spawn } from "node:child_process";
import { fileURLToPath } from "node:url";
import { WebSocket } from "ws";

const serverDirectory = fileURLToPath(new URL("../", import.meta.url));
const port = 18791;
const token = "local-e2e-token";
const child = spawn(process.execPath, ["src/server.mjs"], {
  cwd: serverDirectory,
  env: {
    ...process.env,
    PORT: String(port),
    CAT_PUBLISH_TOKEN: token,
  },
  stdio: ["ignore", "pipe", "pipe"],
  windowsHide: true,
});

let output = "";
child.stdout.on("data", (chunk) => {
  output += chunk.toString();
});
child.stderr.on("data", (chunk) => {
  output += chunk.toString();
});

function open(socket) {
  return new Promise((resolve, reject) => {
    socket.once("open", resolve);
    socket.once("error", reject);
  });
}

function message(socket) {
  return new Promise((resolve, reject) => {
    socket.once("message", (data) => resolve(data.toString()));
    socket.once("error", reject);
  });
}

async function waitForServer() {
  for (let attempt = 0; attempt < 30; attempt += 1) {
    try {
      const response = await fetch(`http://127.0.0.1:${port}/health`);
      if (response.ok) return;
    } catch {
      // 服务仍在启动。
    }
    await new Promise((resolve) => setTimeout(resolve, 100));
  }
  throw new Error(`server did not start\n${output}`);
}

try {
  await waitForServer();

  const viewer = new WebSocket(
    `ws://127.0.0.1:${port}/ws?device=cat-01&role=viewer`,
  );
  const waiting = message(viewer);
  await open(viewer);
  await waiting;

  const device = new WebSocket(
    `ws://127.0.0.1:${port}/ws?device=cat-01&role=device&token=${token}`,
  );
  const ready = message(device);
  await open(device);
  await ready;

  const stateMessage = message(viewer);
  device.send(
    JSON.stringify({
      type: "state",
      protocol: 1,
      deviceId: "cat-01",
      state: 3,
      batteryPercent: 73,
      seq: 42,
      sourceMs: 123456,
    }),
  );

  const received = JSON.parse(await stateMessage);
  if (
    received.state !== 3 ||
    received.name !== "vigorous_activity" ||
    received.batteryPercent !== 73
  ) {
    throw new Error(`unexpected state: ${JSON.stringify(received)}`);
  }

  viewer.close();
  device.close();
  console.log(JSON.stringify(received));
} finally {
  child.kill();
}
