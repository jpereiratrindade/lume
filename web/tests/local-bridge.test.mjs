import assert from "node:assert/strict";
import { spawn } from "node:child_process";
import { mkdtemp, rm } from "node:fs/promises";
import { tmpdir } from "node:os";
import { dirname, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import test from "node:test";

const webRoot = resolve(dirname(fileURLToPath(import.meta.url)), "..");
const repository = resolve(webRoot, "..");
const bridgeScript = resolve(webRoot, "scripts/local-bridge.mjs");
const binary = resolve(repository, "build/lume");
const allowedOrigin = "http://127.0.0.1:3000";

function waitForBridge(child) {
  return new Promise((resolvePromise, rejectPromise) => {
    let output = "";
    let errors = "";
    const timer = setTimeout(() => {
      rejectPromise(new Error(`A ponte não iniciou a tempo. ${errors}`));
    }, 10_000);

    child.stdout.setEncoding("utf8");
    child.stderr.setEncoding("utf8");
    child.stderr.on("data", (chunk) => { errors += chunk; });
    child.stdout.on("data", (chunk) => {
      output += chunk;
      const match = output.match(/http:\/\/127\.0\.0\.1:(\d+)/);
      if (!match) return;
      clearTimeout(timer);
      resolvePromise(`http://127.0.0.1:${match[1]}`);
    });
    child.once("exit", (code) => {
      clearTimeout(timer);
      rejectPromise(new Error(`A ponte encerrou antes de iniciar (código ${code}). ${errors}`));
    });
  });
}

async function jsonRequest(url, options = {}) {
  const response = await fetch(url, {
    ...options,
    headers: { origin: allowedOrigin, ...options.headers },
  });
  return { response, body: await response.json() };
}

test("a ponte local preserva o contrato estruturado do runtime", async (context) => {
  const temporary = await mkdtemp(resolve(tmpdir(), "lume-bridge-test-"));
  const child = spawn(process.execPath, [bridgeScript], {
    cwd: webRoot,
    env: {
      ...process.env,
      LUME_BINARY: binary,
      LUME_BRIDGE_PORT: "0",
      LUME_WEB_STATE_FILE: resolve(temporary, "state.lume"),
      LUME_LLM: "off",
    },
    stdio: ["ignore", "pipe", "pipe"],
  });

  context.after(async () => {
    if (child.exitCode === null) child.kill("SIGTERM");
    await new Promise((resolvePromise) => child.exitCode === null
      ? child.once("exit", resolvePromise)
      : resolvePromise());
    await rm(temporary, { recursive: true, force: true });
  });

  const baseUrl = await waitForBridge(child);
  const health = await jsonRequest(`${baseUrl}/api/health`);
  assert.equal(health.response.status, 200);
  assert.equal(health.body.status, "ok");

  const saved = await jsonRequest(`${baseUrl}/api/say`, {
    method: "POST",
    headers: { "content-type": "application/json" },
    body: JSON.stringify({ text: "Amanhã de manhã quero trabalhar no artigo." }),
  });
  assert.equal(saved.response.status, 200);
  assert.deepEqual(Object.keys(saved.body).sort(), ["changed", "decision", "message", "reason", "type"]);
  assert.equal(saved.body.decision, "REMEMBERED");
  assert.equal(saved.body.changed, true);

  const planResponse = await jsonRequest(`${baseUrl}/api/plan`, {
    method: "POST",
    headers: { "content-type": "application/json" },
    body: JSON.stringify({ horizon: "morning" }),
  });
  assert.equal(planResponse.response.status, 200);
  assert.equal(planResponse.body.type, "plan_proposal");
  assert.ok(planResponse.body.plan_proposal);
  assert.equal(planResponse.body.plan_proposal.status, "draft");

  const applyResponse = await jsonRequest(`${baseUrl}/api/apply-plan`, {
    method: "POST",
    headers: { "content-type": "application/json" },
    body: JSON.stringify({ plan_id: planResponse.body.plan_proposal.id }),
  });
  assert.equal(applyResponse.response.status, 200);
  assert.equal(applyResponse.body.decision, "PLAN_APPLIED");

  const inspected = await jsonRequest(`${baseUrl}/api/inspect`);
  assert.equal(inspected.response.status, 200);
  assert.equal(inspected.body.intentions.length, 1);
  assert.equal(inspected.body.intentions[0].subject, "trabalhar no artigo");

  // Test automation endpoints
  const createAuto = await jsonRequest(`${baseUrl}/api/automations/create`, {
    method: "POST",
    headers: { "content-type": "application/json" },
    body: JSON.stringify({
      title: "Organização Matinal",
      trigger_when: "08:30",
      condition_if: "has_open_intentions",
      action_then: "propose_daily_plan",
      authority: "prepare_proposal",
    }),
  });
  assert.equal(createAuto.response.status, 200);
  assert.equal(createAuto.body.decision, "AUTOMATION_CREATED");

  const automationsList = await jsonRequest(`${baseUrl}/api/automations`);
  assert.equal(automationsList.response.status, 200);
  assert.equal(automationsList.body.automations.length, 1);
  assert.equal(automationsList.body.automations[0].title, "Organização Matinal");

  const tickResponse = await jsonRequest(`${baseUrl}/api/tick`, {
    method: "POST",
    headers: { "content-type": "application/json" },
    body: JSON.stringify({ at: "2026-09-22T08:30:00" }),
  });
  assert.equal(tickResponse.response.status, 200);
  assert.equal(tickResponse.body.decision, "TICK_TRIGGERED");
  assert.ok(tickResponse.body.plan_proposal);

  const denied = await fetch(`${baseUrl}/api/inspect`, {
    headers: { origin: "https://example.invalid" },
  });
  assert.equal(denied.status, 403);
});
