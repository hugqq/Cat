const fs = require("node:fs");
const path = require("node:path");

function readEnvFile() {
  const envPath = path.join(__dirname, ".env");
  const env = {};

  if (!fs.existsSync(envPath)) return env;

  for (const line of fs.readFileSync(envPath, "utf8").split(/\r?\n/)) {
    const trimmed = line.trim();
    if (!trimmed || trimmed.startsWith("#")) continue;

    const separator = trimmed.indexOf("=");
    if (separator < 1) continue;

    const key = trimmed.slice(0, separator).trim();
    const value = trimmed.slice(separator + 1).trim();
    if (/^[A-Za-z_][A-Za-z0-9_]*$/.test(key)) env[key] = value;
  }

  return env;
}

module.exports = {
  apps: [
    {
      name: "cat-state-ws",
      script: "src/server.mjs",
      cwd: __dirname,
      instances: 1,
      exec_mode: "fork",
      autorestart: true,
      watch: false,
      max_memory_restart: "150M",
      env: {
        NODE_ENV: "production",
        PORT: "8787",
        ...readEnvFile(),
      },
    },
  ],
};
