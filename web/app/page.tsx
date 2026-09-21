"use client";

import { FormEvent, KeyboardEvent, useEffect, useMemo, useRef, useState } from "react";

type Role = "person" | "lume";
type Connection = "checking" | "local" | "preview";

type Message = {
  id: number;
  role: Role;
  text: string;
  reason?: string;
};

type Intention = {
  id: number;
  subject: string;
  window_start: string;
  window_end: string;
  precision: string;
  authority: string;
  interpretation_source: string;
  status: string;
  last_interaction_at: string | null;
};

type LumeState = {
  intentions: Intention[];
  interactions: Array<{
    id: number;
    message: string;
    reason: string;
    formulation_source: string;
  }>;
};

type LumeOutcome = {
  decision: string;
  message: string;
  reason: string;
  changed: boolean;
};

const bridge = process.env.NEXT_PUBLIC_LUME_BRIDGE_URL ?? "http://127.0.0.1:4141";

function canUseLocalBridge() {
  return ["localhost", "127.0.0.1", "::1"].includes(window.location.hostname);
}

function outcomeAwaitsReply(outcome: LumeOutcome) {
  return outcome.decision === "INTERACT" || outcome.decision === "CLARIFY";
}

function humanDate(date: Date) {
  return new Intl.DateTimeFormat("pt-BR", {
    weekday: "long",
    day: "numeric",
    month: "long",
  }).format(date);
}

function greeting(date: Date) {
  const hour = date.getHours();
  if (hour < 12) return "Bom dia.";
  if (hour < 18) return "Boa tarde.";
  return "Boa noite.";
}

function extractSubject(value: string) {
  return value
    .replace(/[.!?]+$/g, "")
    .replace(/^amanhã\s+(de manhã|cedo|cedinho)\s+/i, "")
    .replace(/^eu\s+(gostaria|queria)\s+de\s+/i, "")
    .replace(/^quero\s+/i, "")
    .trim();
}

function shortWindow(value: string) {
  if (!value) return "sem horário definido";
  const date = new Date(value);
  if (Number.isNaN(date.getTime())) return "horário indisponível";
  return new Intl.DateTimeFormat("pt-BR", {
    weekday: "short",
    hour: "2-digit",
    minute: "2-digit",
  }).format(date);
}

export default function Home() {
  const [connection, setConnection] = useState<Connection>("checking");
  const [messages, setMessages] = useState<Message[]>([]);
  const [intentions, setIntentions] = useState<Intention[]>([]);
  const [draft, setDraft] = useState("");
  const [processing, setProcessing] = useState(false);
  const [awaitingReply, setAwaitingReply] = useState(false);
  const [contextOpen, setContextOpen] = useState(false);
  const [whyOpen, setWhyOpen] = useState<number | null>(null);
  const [now, setNow] = useState<Date | null>(null);
  const composer = useRef<HTMLTextAreaElement>(null);
  const nextId = useRef(1);

  const openIntentions = useMemo(
    () => intentions.filter((item) => item.status === "open" || item.status === "active"),
    [intentions],
  );

  async function refreshContext() {
    try {
      const response = await fetch(`${bridge}/api/inspect`);
      if (!response.ok) return;
      const state = (await response.json()) as LumeState;
      setIntentions(state.intentions ?? []);
    } catch {
      // The preview remains useful without the local bridge.
    }
  }

  useEffect(() => {
    let active = true;
    const initialClock = window.setTimeout(() => setNow(new Date()), 0);
    async function connect() {
      if (!canUseLocalBridge()) {
        setConnection("preview");
        return;
      }
      try {
        const response = await fetch(`${bridge}/api/health`);
        if (!response.ok) throw new Error("bridge unavailable");
        if (!active) return;
        setConnection("local");
        await refreshContext();

        const observation = await fetch(`${bridge}/api/observe`, { method: "POST" });
        if (observation.ok) {
          const result = (await observation.json()) as LumeOutcome;
          if (result.message && result.message !== "NO_INTERACTION") {
            setMessages([{ id: nextId.current++, role: "lume", text: result.message, reason: result.reason }]);
            setAwaitingReply(outcomeAwaitsReply(result));
          }
        }
      } catch {
        if (active) setConnection("preview");
      }
    }
    connect();
    const timer = window.setInterval(() => setNow(new Date()), 60_000);
    return () => {
      active = false;
      window.clearTimeout(initialClock);
      window.clearInterval(timer);
    };
  }, []);

  function append(role: Role, text: string, reason?: string) {
    setMessages((current) => [...current, { id: nextId.current++, role, text, reason }]);
  }

  async function localAction(text: string) {
    const command = awaitingReply ? "reply" : "say";
    const response = await fetch(`${bridge}/api/${command}`, {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ text }),
    });
    if (!response.ok) throw new Error("local runtime unavailable");
    const result = (await response.json()) as LumeOutcome;
    if (result.message) append("lume", result.message, result.reason);
    setAwaitingReply(outcomeAwaitsReply(result));
    await refreshContext();
  }

  async function previewAction(text: string) {
    await new Promise((resolve) => window.setTimeout(resolve, 700));
    const normalized = text.toLocaleLowerCase("pt-BR");
    if (normalized.includes("amanhã") && /(manhã|cedo|cedinho)/.test(normalized)) {
      const subject = extractSubject(text);
      append(
        "lume",
        "Certo. Amanhã de manhã eu trago isso de volta.",
        "Você expressou uma intenção para amanhã de manhã. Nesta prévia, o estado vive apenas durante a sessão.",
      );
      const tomorrow = new Date(now ?? new Date());
      tomorrow.setDate(tomorrow.getDate() + 1);
      tomorrow.setHours(6, 0, 0, 0);
      setIntentions((current) => [
        ...current,
        {
          id: Date.now(),
          subject,
          window_start: tomorrow.toISOString(),
          window_end: new Date(tomorrow.getTime() + 6 * 60 * 60 * 1000).toISOString(),
          precision: "date+period",
          authority: "user",
          interpretation_source: "preview-language",
          status: "open",
          last_interaction_at: null,
        },
      ]);
      return;
    }
    if (awaitingReply && normalized.includes("daqui a uma hora")) {
      append("lume", "Certo. Daqui a uma hora eu trago isso de volta.");
      setAwaitingReply(false);
      return;
    }
    if (awaitingReply && /^sim[.!]?$/i.test(text.trim())) {
      append("lume", "Certo. Considero isso teu foco agora.");
      setIntentions((current) =>
        current.map((item, index) => (index === current.length - 1 ? { ...item, status: "active" } : item)),
      );
      setAwaitingReply(false);
      return;
    }
    append(
      "lume",
      "Certo. Guardei exatamente como você disse.",
      "Ainda não há informação suficiente para decidir quando agir — e tudo bem.",
    );
  }

  async function send(event?: FormEvent) {
    event?.preventDefault();
    const text = draft.trim();
    if (!text || processing) return;
    append("person", text);
    setDraft("");
    setProcessing(true);
    try {
      if (connection === "local") await localAction(text);
      else await previewAction(text);
    } catch (error) {
      if (connection === "local") {
        setConnection("preview");
        setAwaitingReply(false);
        append(
          "lume",
          "Perdi a conexão com o runtime local. Esta última mensagem não foi salva; reinicie a experiência local e tente novamente.",
          error instanceof Error ? error.message : "A ponte local ficou indisponível.",
        );
      } else {
        await previewAction(text);
      }
    } finally {
      setProcessing(false);
      window.setTimeout(() => composer.current?.focus(), 50);
    }
  }

  function handleKeyDown(event: KeyboardEvent<HTMLTextAreaElement>) {
    if (event.key === "Enter" && !event.shiftKey) {
      event.preventDefault();
      void send();
    }
    if (event.key === "Escape" && contextOpen) setContextOpen(false);
  }

  const lastReason = [...messages].reverse().find((item) => item.role === "lume" && item.reason);
  const hasConversation = messages.length > 0;
  const today = now ? humanDate(now) : "Hoje";
  const welcome = now ? greeting(now) : "Olá.";

  function returnHome() {
    setMessages([]);
    setAwaitingReply(false);
    setWhyOpen(null);
    setDraft("");
  }

  return (
    <main className={`lume-shell ${hasConversation ? "is-conversation" : ""}`}>
      <header className="topbar">
        <button className="brand" type="button" onClick={returnHome} aria-label="Lume — início">
          <span className="brand-mark" aria-hidden="true" />
          <span>lume</span>
        </button>
        <button className={`presence ${connection}`} type="button" onClick={() => setContextOpen(true)}>
          <span className="presence-dot" aria-hidden="true" />
          {connection === "checking" ? "aproximando" : connection === "local" ? "local e presente" : "prévia de experiência"}
        </button>
      </header>

      {!hasConversation ? (
        <section className="presence-stage" aria-labelledby="greeting">
          <div className="ambient-glow" aria-hidden="true" />
          <p className="eyebrow">{today}</p>
          <h1 id="greeting">{welcome}</h1>
          <p className="opening">O que está acontecendo agora?</p>
          <Composer
            draft={draft}
            setDraft={setDraft}
            processing={processing}
            onSubmit={send}
            onKeyDown={handleKeyDown}
            inputRef={composer}
          />
          <div className="suggestions" aria-label="Exemplos do que dizer">
            {["Amanhã cedo quero voltar ao artigo", "Isso não é urgente, mas não pode sumir"].map((suggestion) => (
              <button key={suggestion} type="button" onClick={() => { setDraft(suggestion); composer.current?.focus(); }}>
                {suggestion}
              </button>
            ))}
          </div>
          <p className="quiet-state">
            <span aria-hidden="true">✦</span>
            {openIntentions.length === 0
              ? "Por enquanto, nada pede tua atenção."
              : `${openIntentions.length} ${openIntentions.length === 1 ? "intenção permanece aberta" : "intenções permanecem abertas"}.`}
          </p>
        </section>
      ) : (
        <section className="conversation-stage" aria-label="Conversa com Lume">
          <div className="conversation-heading">
            <p>{today}</p>
            <h1>Estou aqui.</h1>
          </div>
          <div className="messages" aria-live="polite">
            {messages.map((message) => (
              <article className={`message ${message.role}`} key={message.id}>
                <p>{message.text}</p>
                {message.reason && (
                  <button type="button" onClick={() => setWhyOpen(whyOpen === message.id ? null : message.id)}>
                    {whyOpen === message.id ? "Ocultar motivo" : "Por que agora?"}
                  </button>
                )}
                {whyOpen === message.id && message.reason && (
                  <div className="reason"><span>Razão concreta</span>{message.reason}</div>
                )}
              </article>
            ))}
            {processing && (
              <div className="thinking" aria-label="Lume está pensando"><i /><i /><i /></div>
            )}
          </div>
          <div className="conversation-composer">
            <Composer
              draft={draft}
              setDraft={setDraft}
              processing={processing}
              onSubmit={send}
              onKeyDown={handleKeyDown}
              inputRef={composer}
              compact
            />
            <p>Enter envia · Shift + Enter cria uma linha</p>
          </div>
        </section>
      )}

      <footer className="bottom-note">
        <span>Seu contexto fica com você.</span>
        <button type="button" onClick={() => setContextOpen(true)}>
          <span className="context-count">{openIntentions.length}</span>
          Ver contexto
        </button>
      </footer>

      {contextOpen && (
        <div className="drawer-layer" role="presentation" onMouseDown={(event) => {
          if (event.target === event.currentTarget) setContextOpen(false);
        }}>
          <aside className="context-drawer" aria-label="Contexto do Lume">
            <div className="drawer-header">
              <div>
                <p>Contexto</p>
                <h2>O que o Lume está carregando</h2>
              </div>
              <button type="button" onClick={() => setContextOpen(false)} aria-label="Fechar contexto">×</button>
            </div>

            <section className="context-section">
              <div className="section-label"><span className="status-orb" /> Agora</div>
              {openIntentions.length === 0 ? (
                <p className="empty-context">Tudo tranquilo. Nenhuma intenção precisa de ação agora.</p>
              ) : (
                <div className="intention-list">
                  {openIntentions.map((item) => (
                    <article className="intention-card" key={item.id}>
                      <div>
                        <strong>{item.subject}</strong>
                        <span>{item.status === "active" ? "foco atual" : shortWindow(item.window_start)}</span>
                      </div>
                      <span className={`intention-status ${item.status}`}>{item.status === "active" ? "em foco" : "aberta"}</span>
                    </article>
                  ))}
                </div>
              )}
            </section>

            <section className="context-section soft">
              <div className="section-label">Como isto foi entendido</div>
              <dl className="facts">
                <div><dt>Autoridade</dt><dd>{openIntentions.at(-1)?.authority ?? "nenhuma ação concedida"}</dd></div>
                <div><dt>Interpretação</dt><dd>{openIntentions.at(-1)?.interpretation_source ?? "aguardando expressão"}</dd></div>
                <div><dt>Precisão</dt><dd>{openIntentions.at(-1)?.precision ?? "—"}</dd></div>
              </dl>
              {lastReason?.reason && <p className="last-reason"><span>Última razão</span>{lastReason.reason}</p>}
            </section>

            <div className="connection-note">
              <span className={`connection-icon ${connection}`} aria-hidden="true" />
              <div>
                <strong>{connection === "local" ? "Runtime local conectado" : "Prévia sem estado canônico"}</strong>
                <p>{connection === "local"
                  ? "O core C++ decide; esta interface apenas conversa e apresenta contexto."
                  : "As interações desta prévia ficam somente nesta sessão. Execute npm run dev:local para conectar o Lume real."}</p>
              </div>
            </div>
          </aside>
        </div>
      )}
    </main>
  );
}

function Composer({
  draft,
  setDraft,
  processing,
  onSubmit,
  onKeyDown,
  inputRef,
  compact = false,
}: {
  draft: string;
  setDraft: (value: string) => void;
  processing: boolean;
  onSubmit: (event?: FormEvent) => Promise<void>;
  onKeyDown: (event: KeyboardEvent<HTMLTextAreaElement>) => void;
  inputRef: React.RefObject<HTMLTextAreaElement | null>;
  compact?: boolean;
}) {
  return (
    <form className={`expression ${compact ? "compact" : ""}`} onSubmit={(event) => void onSubmit(event)}>
      <label className="sr-only" htmlFor={compact ? "thought-compact" : "thought"}>Conte ao Lume o que está acontecendo</label>
      <textarea
        ref={inputRef}
        id={compact ? "thought-compact" : "thought"}
        rows={compact ? 1 : 2}
        value={draft}
        onChange={(event) => setDraft(event.target.value)}
        onKeyDown={onKeyDown}
        placeholder={compact ? "Diga o que mudou…" : "Pode falar do teu jeito…"}
        disabled={processing}
      />
      <button type="submit" aria-label="Enviar para o Lume" disabled={!draft.trim() || processing}>
        <span aria-hidden="true">↑</span>
      </button>
    </form>
  );
}
