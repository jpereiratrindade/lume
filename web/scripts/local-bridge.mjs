import { createServer } from "node:http";
import { spawn } from "node:child_process";
import { existsSync } from "node:fs";
import { dirname, resolve } from "node:path";
import { fileURLToPath } from "node:url";

const here = dirname(fileURLToPath(import.meta.url));
const repository = resolve(here, "../..");
const binary = process.env.LUME_BINARY || resolve(repository, "build/lume");
const host = "127.0.0.1";
const port = parsePort(process.env.LUME_BRIDGE_PORT, 4141, true);
const webPort = parsePort(process.env.LUME_WEB_PORT, 3000);
const childEnvironment = { ...process.env };
if (process.env.LUME_WEB_STATE_FILE) childEnvironment.LUME_STATE_FILE = process.env.LUME_WEB_STATE_FILE;
const allowedOrigins = new Set([
  `http://localhost:${webPort}`,
  `http://127.0.0.1:${webPort}`,
]);

function parsePort(value, fallback, allowZero = false) {
  const candidate = value === undefined ? fallback : Number(value);
  const minimum = allowZero ? 0 : 1;
  if (!Number.isInteger(candidate) || candidate < minimum || candidate > 65_535) {
    throw new Error(`Porta inválida: ${value}`);
  }
  return candidate;
}

if (!existsSync(binary)) {
  console.error(`Lume não foi encontrado em ${binary}. Compile o projeto C++ primeiro.`);
  process.exit(1);
}

function runLume(args, timeout = 120_000) {
  return new Promise((resolvePromise, rejectPromise) => {
    const child = spawn(binary, args, {
      cwd: repository,
      env: childEnvironment,
      stdio: ["ignore", "pipe", "pipe"],
    });
    let stdout = "";
    let stderr = "";
    const timer = setTimeout(() => {
      child.kill("SIGTERM");
      rejectPromise(new Error("O runtime local excedeu o tempo de resposta."));
    }, timeout);
    child.stdout.setEncoding("utf8");
    child.stderr.setEncoding("utf8");
    child.stdout.on("data", (chunk) => { stdout += chunk; });
    child.stderr.on("data", (chunk) => { stderr += chunk; });
    child.on("error", rejectPromise);
    child.on("close", (code) => {
      clearTimeout(timer);
      if (code === 0) resolvePromise(stdout.trim());
      else rejectPromise(new Error(stderr.trim() || `Lume encerrou com código ${code}.`));
    });
  });
}

function isLoopback(address) {
  return address === "127.0.0.1" || address === "::1" || address === "::ffff:127.0.0.1";
}

function corsHeaders(request) {
  const origin = request.headers.origin;
  return origin && allowedOrigins.has(origin)
    ? { "Access-Control-Allow-Origin": origin, Vary: "Origin" }
    : {};
}

function send(request, response, status, payload) {
  response.writeHead(status, {
    "Content-Type": "application/json; charset=utf-8",
    "Cache-Control": "no-store",
    ...corsHeaders(request),
  });
  response.end(JSON.stringify(payload));
}

async function readBody(request) {
  let body = "";
  for await (const chunk of request) {
    body += chunk;
    if (body.length > 16_384) throw new Error("Expressão grande demais.");
  }
  return body ? JSON.parse(body) : {};
}

function parseOutcome(output) {
  const outcome = JSON.parse(output);
  if (
    !outcome ||
    typeof outcome.decision !== "string" ||
    typeof outcome.message !== "string" ||
    typeof outcome.reason !== "string" ||
    typeof outcome.changed !== "boolean"
  ) {
    throw new Error("O runtime local devolveu uma resposta inválida.");
  }
  return outcome;
}

const server = createServer(async (request, response) => {
  if (!isLoopback(request.socket.remoteAddress)) {
    send(request, response, 403, { error: "A ponte do Lume aceita somente conexões locais." });
    return;
  }
  const origin = request.headers.origin;
  if (origin && !allowedOrigins.has(origin)) {
    send(request, response, 403, { error: "Origem não autorizada." });
    return;
  }
  if (request.method === "OPTIONS") {
    response.writeHead(204, {
      ...corsHeaders(request),
      "Access-Control-Allow-Methods": "GET, POST, OPTIONS",
      "Access-Control-Allow-Headers": "Content-Type",
      "Access-Control-Max-Age": "600",
    });
    response.end();
    return;
  }

  try {
    if (request.method === "GET" && request.url === "/api/health") {
      const output = await runLume(["doctor"], 5_000);
      send(request, response, 200, { status: "ok", detail: output.split("\n")[0] });
      return;
    }
    if (request.method === "GET" && request.url === "/api/inspect") {
      send(request, response, 200, JSON.parse(await runLume(["inspect"], 5_000)));
      return;
    }
    if (request.method === "POST" && request.url === "/api/observe") {
      send(request, response, 200, parseOutcome(await runLume(["observe", "--json"])));
      return;
    }
    if (request.method === "POST" && ["/api/say", "/api/reply"].includes(request.url || "")) {
      const body = await readBody(request);
      if (typeof body.text !== "string" || !body.text.trim()) {
        send(request, response, 400, { error: "Uma expressão é necessária." });
        return;
      }
      const command = request.url === "/api/reply" ? "reply" : "say";
      send(request, response, 200, parseOutcome(await runLume([command, body.text.trim(), "--json"])));
      return;
    }
    send(request, response, 404, { error: "Recurso não encontrado." });
  } catch (error) {
    send(request, response, 500, { error: error instanceof Error ? error.message : "Falha no runtime local." });
  }
});

server.listen(port, host, () => {
  const address = server.address();
  const listeningPort = typeof address === "object" && address ? address.port : port;
  console.log(`Ponte local do Lume em http://${host}:${listeningPort}`);
});
