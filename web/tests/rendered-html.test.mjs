import assert from "node:assert/strict";
import test from "node:test";

async function render() {
  const workerUrl = new URL("../dist/server/index.js", import.meta.url);
  workerUrl.searchParams.set("test", `${process.pid}-${Date.now()}`);
  const { default: worker } = await import(workerUrl.href);
  return worker.fetch(
    new Request("https://lume.example/", { headers: { accept: "text/html", host: "lume.example" } }),
    { ASSETS: { fetch: async () => new Response("Not found", { status: 404 }) } },
    { waitUntil() {}, passThroughOnException() {} },
  );
}

test("server-renders the Lume presence surface", async () => {
  const response = await render();
  assert.equal(response.status, 200);
  assert.match(response.headers.get("content-type") ?? "", /^text\/html\b/i);
  const html = await response.text();
  assert.match(html, /<title>Lume — seu contexto, presente<\/title>/i);
  assert.match(html, /<p class="eyebrow">Hoje<\/p>/);
  assert.match(html, /<h1 id="greeting">Olá\.<\/h1>/);
  assert.match(html, /O que está acontecendo agora\?/);
  assert.match(html, /Pode falar do teu jeito/);
  assert.match(html, /Seu contexto fica com você/);
  assert.match(html, /https:\/\/lume\.example\/og\.png/);
  assert.doesNotMatch(html, /codex-preview|SkeletonPreview|Starter Project/);
});
