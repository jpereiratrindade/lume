import { createServer } from "node:http";
import { spawn } from "node:child_process";
import { existsSync } from "node:fs";
import { dirname, resolve } from "node:path";
import { fileURLToPath } from "node:url";

const here = dirname(fileURLToPath(import.meta.url));
const repository = resolve(here, "../..");
const binary = process.env.LUME_BINARY || resolve(repository, "build/lume");
const host = "0.0.0.0";
const port = parsePort(process.env.LUME_BRIDGE_PORT, 4141, true);
const webPort = parsePort(process.env.LUME_WEB_PORT, 3000);
const childEnvironment = { ...process.env };
if (process.env.LUME_WEB_STATE_FILE) childEnvironment.LUME_STATE_FILE = process.env.LUME_WEB_STATE_FILE;
if (!childEnvironment.LUME_LLM) childEnvironment.LUME_LLM = "deterministic";

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
  if (!address) return false;
  return (
    address === "127.0.0.1" ||
    address === "::1" ||
    address === "::ffff:127.0.0.1" ||
    address.startsWith("127.")
  );
}

function corsHeaders(request) {
  const origin = request.headers.origin;
  if (!origin) return {};
  try {
    const parsed = new URL(origin);
    if (["localhost", "127.0.0.1", "::1", "[::1]"].includes(parsed.hostname)) {
      return {
        "Access-Control-Allow-Origin": origin,
        "Access-Control-Allow-Methods": "GET, POST, OPTIONS",
        "Access-Control-Allow-Headers": "Content-Type",
        Vary: "Origin",
      };
    }
  } catch {}
  return {};
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
    if (request.method === "POST" && request.url === "/api/plan") {
      const body = await readBody(request);
      const horizon = typeof body.horizon === "string" && body.horizon.trim() ? body.horizon.trim() : "morning";
      send(request, response, 200, parseOutcome(await runLume(["plan", horizon, "--json"])));
      return;
    }
    if (request.method === "POST" && request.url === "/api/apply-plan") {
      const body = await readBody(request);
      if (typeof body.plan_id !== "number") {
        send(request, response, 400, { error: "ID de proposta inválido." });
        return;
      }
      send(request, response, 200, parseOutcome(await runLume(["apply-plan", String(body.plan_id), "--json"])));
      return;
    }
    if (request.method === "POST" && request.url === "/api/discard-plan") {
      const body = await readBody(request);
      if (typeof body.plan_id !== "number") {
        send(request, response, 400, { error: "ID de proposta inválido." });
        return;
      }
      send(request, response, 200, parseOutcome(await runLume(["discard-plan", String(body.plan_id), "--json"])));
      return;
    }
    if (request.method === "GET" && request.url === "/api/automations") {
      send(request, response, 200, JSON.parse(await runLume(["automations", "--json"])));
      return;
    }
    if (request.method === "POST" && request.url === "/api/automations/create") {
      const body = await readBody(request);
      const title = typeof body.title === "string" ? body.title.trim() : "Nova Rotina";
      const trigger_when = typeof body.trigger_when === "string" ? body.trigger_when.trim() : "08:30";
      const condition_if = typeof body.condition_if === "string" ? body.condition_if.trim() : "has_open_intentions";
      const action_then = typeof body.action_then === "string" ? body.action_then.trim() : "propose_daily_plan";
      const authority = typeof body.authority === "string" ? body.authority.trim() : "suggest_only";
      send(request, response, 200, parseOutcome(await runLume(["create-automation", title, trigger_when, condition_if, action_then, authority, "--json"])));
      return;
    }
    if (request.method === "POST" && request.url === "/api/intentions/create") {
      const body = await readBody(request);
      if (typeof body.subject !== "string" || !body.subject.trim()) {
        send(request, response, 400, { error: "Assunto da intenção é obrigatório." });
        return;
      }
      const args = ["create-intention", body.subject.trim()];
      if (typeof body.start === "string" && body.start.trim()) args.push(body.start.trim());
      if (typeof body.end === "string" && body.end.trim()) args.push(body.end.trim());
      args.push("--json");
      send(request, response, 200, parseOutcome(await runLume(args)));
      return;
    }
    if (request.method === "POST" && request.url === "/api/intentions/status") {
      const body = await readBody(request);
      if (typeof body.id !== "number" || typeof body.status !== "string") {
        send(request, response, 400, { error: "ID e status são obrigatórios." });
        return;
      }
      const cmd = body.status === "completed" ? "complete-intention" : "dismiss-intention";
      send(request, response, 200, parseOutcome(await runLume([cmd, String(body.id), "--json"])));
      return;
    }
    if (request.method === "POST" && request.url === "/api/intentions/defer") {
      const body = await readBody(request);
      if (typeof body.id !== "number") {
        send(request, response, 400, { error: "ID da intenção é obrigatório." });
        return;
      }
      const args = ["defer-intention", String(body.id)];
      if (typeof body.start === "string" && body.start.trim()) args.push(body.start.trim());
      if (typeof body.end === "string" && body.end.trim()) args.push(body.end.trim());
      args.push("--json");
      send(request, response, 200, parseOutcome(await runLume(args)));
      return;
    }
    if (request.method === "POST" && request.url === "/api/automations/toggle") {
      const body = await readBody(request);
      if (typeof body.id !== "number" || typeof body.status !== "string") {
        send(request, response, 400, { error: "Parâmetros de automação inválidos." });
        return;
      }
      send(request, response, 200, parseOutcome(await runLume(["toggle-automation", String(body.id), body.status.trim(), "--json"])));
      return;
    }
    if (request.method === "POST" && request.url === "/api/automations/trigger") {
      const body = await readBody(request);
      if (typeof body.id !== "number") {
        send(request, response, 400, { error: "Identificador de automação inválido." });
        return;
      }
      send(request, response, 200, parseOutcome(await runLume(["trigger-automation", String(body.id), "--json"])));
      return;
    }
    if (request.method === "POST" && request.url === "/api/tick") {
      const body = await readBody(request);
      const args = ["tick", "--json"];
      if (typeof body.at === "string" && body.at.trim()) {
        args.push("--at", body.at.trim());
      }
      send(request, response, 200, parseOutcome(await runLume(args)));
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
