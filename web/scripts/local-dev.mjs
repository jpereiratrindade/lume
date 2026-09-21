import { spawn } from "node:child_process";
import { dirname, resolve } from "node:path";
import { fileURLToPath } from "node:url";

const webRoot = resolve(dirname(fileURLToPath(import.meta.url)), "..");
const bridgePort = parsePort(process.env.LUME_BRIDGE_PORT, 4141);
const webPort = parsePort(process.env.LUME_WEB_PORT, 3000);
const childEnvironment = {
  ...process.env,
  LUME_BRIDGE_PORT: String(bridgePort),
  LUME_WEB_PORT: String(webPort),
  LUME_LLM: process.env.LUME_LLM || "deterministic",
  NEXT_PUBLIC_LUME_BRIDGE_URL: `http://127.0.0.1:${bridgePort}`,
};
const children = [
  spawn(process.execPath, ["--watch", resolve(webRoot, "scripts/local-bridge.mjs")], {
    cwd: webRoot,
    env: childEnvironment,
    stdio: "inherit",
  }),
  spawn("npm", ["run", "dev"], { cwd: webRoot, env: childEnvironment, stdio: "inherit" }),
];

function parsePort(value, fallback) {
  const candidate = value === undefined ? fallback : Number(value);
  if (!Number.isInteger(candidate) || candidate < 1 || candidate > 65_535) {
    throw new Error(`Porta inválida: ${value}`);
  }
  return candidate;
}

let stopping = false;
function stop(code = 0) {
  if (stopping) return;
  stopping = true;
  for (const child of children) child.kill("SIGTERM");
  setTimeout(() => process.exit(code), 100).unref();
}

for (const child of children) {
  child.on("exit", (code) => {
    if (!stopping) stop(code ?? 1);
  });
  child.on("error", () => stop(1));
}

process.on("SIGINT", () => stop(0));
process.on("SIGTERM", () => stop(0));
