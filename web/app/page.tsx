"use client";

import { FormEvent, KeyboardEvent, useCallback, useEffect, useMemo, useRef, useState } from "react";

type Role = "person" | "lume";
type Connection = "checking" | "local" | "preview";
type ViewMode = "presence" | "care" | "plan" | "automate" | "connect" | "analyze";

type PlanBlock = {
  title: string;
  start: string;
  end: string;
  category: string;
  intention_id: number;
};

type PlanProposal = {
  id: number;
  horizon: string;
  summary: string;
  blocks: PlanBlock[];
  points_of_attention: string[];
  status: "draft" | "applied" | "discarded";
  source: string;
};

type Message = {
  id: number;
  role: Role;
  text: string;
  reason?: string;
  plan_proposal?: PlanProposal;
};

type Intention = {
  id: number;
  expression_id?: number;
  subject: string;
  window_start: string;
  window_end: string;
  precision: string;
  authority: string;
  interpretation_source: string;
  status: string;
  last_interaction_at: string | null;
};

type AttentionCandidate = {
  intention_id: number;
  subject: string;
  window_start: string;
  window_end: string;
  relevance_reason: string;
  is_active_now: boolean;
};

type ExpressionRecord = {
  id: number;
  recorded_at: string;
  text: string;
  epistemic_class: string;
};

type LumeState = {
  state_version?: number;
  ledger_events_count?: number;
  attention_candidate?: AttentionCandidate | null;
  expressions?: ExpressionRecord[];
  intentions: Intention[];
  interactions: Array<{
    id: number;
    message: string;
    reason: string;
    formulation_source: string;
  }>;
  plan_proposals?: PlanProposal[];
};

type AutomationProposal = {
  id: number;
  title: string;
  trigger_when: string;
  condition_if: string;
  action_then: string;
  authority: string;
  status: "active" | "paused" | "discarded";
  last_triggered_at: string | null;
};

type NotificationRecord = {
  id: number;
  automation_id: number;
  emitted_at: string;
  title: string;
  message: string;
  action_type: string;
  reference_id?: number;
};

type LumeOutcome = {
  decision: string;
  message: string;
  reason: string;
  changed: boolean;
  type?: string;
  plan_proposal?: PlanProposal;
  automation_proposal?: AutomationProposal;
  emitted_notifications?: NotificationRecord[];
};

function getBridgeUrl() {
  if (typeof window === "undefined") {
    return process.env.NEXT_PUBLIC_LUME_BRIDGE_URL ?? "http://127.0.0.1:4141";
  }
  const host = window.location.hostname === "localhost" ? "localhost" : "127.0.0.1";
  return `http://${host}:4141`;
}

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

function formatBlockTime(value: string) {
  if (!value) return "";
  const date = new Date(value);
  if (Number.isNaN(date.getTime())) return value;
  return new Intl.DateTimeFormat("pt-BR", {
    hour: "2-digit",
    minute: "2-digit",
  }).format(date);
}

function toDateTimeLocal(value: string) {
  const date = new Date(value);
  if (Number.isNaN(date.getTime())) return "";
  const offset = date.getTimezoneOffset() * 60_000;
  return new Date(date.getTime() - offset).toISOString().slice(0, 16);
}

function humanAuthority(value: string) {
  if (value === "suggest_only") return "Pode apenas sugerir";
  if (value === "prepare_proposal") return "Pode preparar, mas espera tua confirmação";
  if (value === "execute") return "Pode executar como autorizado";
  return value;
}

export default function Home() {
  const [connection, setConnection] = useState<Connection>("checking");
  const [viewMode, setViewMode] = useState<ViewMode>("presence");
  const [messages, setMessages] = useState<Message[]>([]);
  const [expressions, setExpressions] = useState<ExpressionRecord[]>([]);
  const [intentions, setIntentions] = useState<Intention[]>([]);
  const [attentionCandidate, setAttentionCandidate] = useState<AttentionCandidate | null>(null);
  const [automations, setAutomations] = useState<AutomationProposal[]>([]);
  const [notifications, setNotifications] = useState<NotificationRecord[]>([]);
  const [ledgerEventsCount, setLedgerEventsCount] = useState<number>(0);
  const [activePlan, setActivePlan] = useState<PlanProposal | null>(null);
  const [processing, setProcessing] = useState(false);
  const [awaitingReply, setAwaitingReply] = useState(false);
  const [whyOpen, setWhyOpen] = useState<number | null>(null);
  const [now, setNow] = useState<Date | null>(null);
  const [statusMessage, setStatusMessage] = useState<string | null>(null);
  const [isNewModalOpen, setIsNewModalOpen] = useState(false);
  const [editingIntentionId, setEditingIntentionId] = useState<number | null>(null);
  const [newSubject, setNewSubject] = useState("");
  const [newStart, setNewStart] = useState("");
  const [newEnd, setNewEnd] = useState("");
  const composer = useRef<HTMLInputElement>(null);
  const nextId = useRef(1);
  const bridge = getBridgeUrl();

  const openIntentions = useMemo(
    () => intentions.filter((item) => item.status === "open" || item.status === "active"),
    [intentions],
  );

  const refreshContext = useCallback(async () => {
    try {
      const response = await fetch(`${bridge}/api/inspect`);
      if (response.ok) {
        const state = (await response.json()) as LumeState;
        setExpressions(state.expressions ?? []);
        setIntentions(state.intentions ?? []);
        setAttentionCandidate(state.attention_candidate ?? null);
        if (typeof state.ledger_events_count === "number") {
          setLedgerEventsCount(state.ledger_events_count);
        }
        if (state.plan_proposals && state.plan_proposals.length > 0) {
          const latestPlan = state.plan_proposals[state.plan_proposals.length - 1];
          if (latestPlan.status === "draft") {
            setActivePlan(latestPlan);
          }
        }
      }
      const autoRes = await fetch(`${bridge}/api/automations`);
      if (autoRes.ok) {
        const autoData = await autoRes.json();
        setAutomations(autoData.automations || []);
        setNotifications(autoData.notifications || []);
      }
    } catch {
      // Offline / preview mode fallback
    }
  }, [bridge]);

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
            setMessages([
              {
                id: nextId.current++,
                role: "lume",
                text: result.message,
                reason: result.reason,
                plan_proposal: result.plan_proposal,
              },
            ]);
            setAwaitingReply(outcomeAwaitsReply(result));
            if (result.plan_proposal) {
              setActivePlan(result.plan_proposal);
            }
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
  }, [bridge, refreshContext]);

  function append(role: Role, text: string, reason?: string, plan_proposal?: PlanProposal) {
    setMessages((current) => [...current, { id: nextId.current++, role, text, reason, plan_proposal }]);
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
    if (result.plan_proposal) {
      setActivePlan(result.plan_proposal);
      setViewMode("plan");
    }
    if (result.message || result.plan_proposal) {
      append("lume", result.message || "Proposta preparada.", result.reason, result.plan_proposal);
    }
    setAwaitingReply(outcomeAwaitsReply(result));
    await refreshContext();
  }

  async function triggerPlan(horizon: "morning" | "week" | "day") {
    setProcessing(true);
    try {
      if (connection === "local") {
        const response = await fetch(`${bridge}/api/plan`, {
          method: "POST",
          headers: { "Content-Type": "application/json" },
          body: JSON.stringify({ horizon }),
        });
        if (!response.ok) throw new Error("Erro ao gerar plano no runtime.");
        const result = (await response.json()) as LumeOutcome;
        if (result.plan_proposal) {
          setActivePlan(result.plan_proposal);
          setViewMode("plan");
          append("lume", result.message, result.reason, result.plan_proposal);
        }
      } else {
        // Preview mode
        const isWeek = horizon === "week";
        const proposal: PlanProposal = {
          id: nextId.current++,
          horizon: isWeek ? "Semana" : "Manhã",
          summary: isWeek
            ? "Preparei uma proposta estruturada para tua semana."
            : "Preparei uma proposta para o teu período da manhã.",
          blocks: isWeek
            ? [
                { title: "Segunda — Pesquisa e foco profundo", start: "2026-09-21T09:00:00", end: "2026-09-21T13:00:00", category: "focus", intention_id: 0 },
                { title: "Terça — Alinhamentos e reuniões", start: "2026-09-22T09:00:00", end: "2026-09-22T13:00:00", category: "meeting", intention_id: 0 },
                { title: "Quarta — Redação e síntese do artigo", start: "2026-09-23T09:00:00", end: "2026-09-23T13:00:00", category: "focus", intention_id: 0 },
                { title: "Quinta — Campo e execuções externas", start: "2026-09-24T09:00:00", end: "2026-09-24T13:00:00", category: "focus", intention_id: 0 },
                { title: "Sexta — Revisão semanal e retrospectiva", start: "2026-09-25T09:00:00", end: "2026-09-25T13:00:00", category: "review", intention_id: 0 },
              ]
            : [
                { title: "Bloco de foco: finalizar o artigo", start: "2026-09-22T09:00:00", end: "2026-09-22T11:00:00", category: "focus", intention_id: 0 },
                { title: "Alinhamentos e revisão", start: "2026-09-22T11:00:00", end: "2026-09-22T12:00:00", category: "review", intention_id: 0 },
              ],
          points_of_attention: isWeek
            ? ["Terça-feira está muito fragmentada com reuniões curtas.", "Quarta-feira possui uma janela contínua protegida de 4h."]
            : ["Intenção alocada no início da manhã para proteger concentração."],
          status: "draft",
          source: "preview-planner",
        };
        setActivePlan(proposal);
        setViewMode("plan");
        append("lume", proposal.summary, "Proposta de planejamento visual gerada.", proposal);
      }
    } catch (err) {
      setStatusMessage(err instanceof Error ? err.message : "Erro ao planejar.");
    } finally {
      setProcessing(false);
    }
  }

  async function handleApplyPlan(planId: number) {
    setProcessing(true);
    try {
      if (connection === "local") {
        const response = await fetch(`${bridge}/api/apply-plan`, {
          method: "POST",
          headers: { "Content-Type": "application/json" },
          body: JSON.stringify({ plan_id: planId }),
        });
        if (!response.ok) throw new Error("Falha ao aplicar proposta no runtime local.");
        const result = (await response.json()) as LumeOutcome;
        if (activePlan && activePlan.id === planId) {
          setActivePlan({ ...activePlan, status: "applied" });
        }
        setStatusMessage("Plano guardado.");
        append("lume", result.message, result.reason);
        await refreshContext();
      } else {
        if (activePlan && activePlan.id === planId) {
          setActivePlan({ ...activePlan, status: "applied" });
        }
        setStatusMessage("Plano aplicado localmente.");
        append("lume", "Certo. Vou considerar este plano daqui em diante.", "Você confirmou o plano.");
      }
    } catch (error) {
      setStatusMessage(error instanceof Error ? error.message : "Erro ao aplicar.");
    } finally {
      setProcessing(false);
    }
  }

  async function handleDiscardPlan(planId: number) {
    setProcessing(true);
    try {
      if (connection === "local") {
        const response = await fetch(`${bridge}/api/discard-plan`, {
          method: "POST",
          headers: { "Content-Type": "application/json" },
          body: JSON.stringify({ plan_id: planId }),
        });
        if (!response.ok) throw new Error("Falha ao descartar proposta.");
        const result = (await response.json()) as LumeOutcome;
        if (activePlan && activePlan.id === planId) {
          setActivePlan({ ...activePlan, status: "discarded" });
        }
        setStatusMessage("Proposta descartada.");
        append("lume", result.message, result.reason);
        await refreshContext();
      } else {
        if (activePlan && activePlan.id === planId) {
          setActivePlan({ ...activePlan, status: "discarded" });
        }
        setStatusMessage("Proposta descartada.");
      }
    } catch (error) {
      setStatusMessage(error instanceof Error ? error.message : "Erro ao descartar.");
    } finally {
      setProcessing(false);
    }
  }

  async function handleCreateAutomation(
    title: string,
    trigger_when: string,
    condition_if: string,
    action_then: string,
    authority: string,
  ) {
    setProcessing(true);
    try {
      if (connection === "local") {
        const response = await fetch(`${bridge}/api/automations/create`, {
          method: "POST",
          headers: { "Content-Type": "application/json" },
          body: JSON.stringify({ title, trigger_when, condition_if, action_then, authority }),
        });
        if (!response.ok) throw new Error("Erro ao registrar automação no runtime.");
        const result = (await response.json()) as LumeOutcome;
        append("lume", result.message, result.reason);
        await refreshContext();
      } else {
        const newAuto: AutomationProposal = {
          id: nextId.current++,
          title,
          trigger_when,
          condition_if,
          action_then,
          authority,
          status: "active",
          last_triggered_at: null,
        };
        setAutomations((prev) => [...prev, newAuto]);
        append("lume", `Rotina "${title}" ativada no ambiente de prévia.`);
      }
    } catch (error) {
      append("lume", "Não foi possível criar a automação.", error instanceof Error ? error.message : "Erro");
    } finally {
      setProcessing(false);
    }
  }

  async function handleToggleAutomation(id: number, currentStatus: string) {
    setProcessing(true);
    const nextStatus = currentStatus === "active" ? "paused" : "active";
    try {
      if (connection === "local") {
        const response = await fetch(`${bridge}/api/automations/toggle`, {
          method: "POST",
          headers: { "Content-Type": "application/json" },
          body: JSON.stringify({ id, status: nextStatus }),
        });
        if (!response.ok) throw new Error("Erro ao alterar status da automação.");
        await refreshContext();
      } else {
        setAutomations((prev) =>
          prev.map((a) => (a.id === id ? { ...a, status: nextStatus as "active" | "paused" } : a))
        );
      }
    } catch (error) {
      append("lume", "Falha ao alterar status.", error instanceof Error ? error.message : "Erro");
    } finally {
      setProcessing(false);
    }
  }

  function closeIntentionModal() {
    setIsNewModalOpen(false);
    setEditingIntentionId(null);
    setNewSubject("");
    setNewStart("");
    setNewEnd("");
  }

  function openEditIntentionModal(intention: Intention) {
    setEditingIntentionId(intention.id);
    setNewSubject(intention.subject);
    setNewStart(toDateTimeLocal(intention.window_start));
    setNewEnd(toDateTimeLocal(intention.window_end));
    setIsNewModalOpen(true);
  }

  async function handleEditIntention(id: number, subject: string, start: string, end: string) {
    if (!subject.trim() || !start || !end) return;
    setProcessing(true);
    try {
      if (connection === "local") {
        const response = await fetch(`${bridge}/api/intentions/update`, {
          method: "POST",
          headers: { "Content-Type": "application/json" },
          body: JSON.stringify({ id, subject: subject.trim(), start, end }),
        });
        if (!response.ok) throw new Error("Erro ao editar intenção.");
        const outcome = (await response.json()) as LumeOutcome;
        append("lume", outcome.message, outcome.reason);
        await refreshContext();
      } else {
        setIntentions((current) => current.map((item) => item.id === id
          ? {
              ...item,
              subject: subject.trim(),
              window_start: new Date(start).toISOString(),
              window_end: new Date(end).toISOString(),
              precision: "direct",
            }
          : item));
      }
    } catch (error) {
      append("lume", "Falha ao editar intenção.", error instanceof Error ? error.message : "Erro");
    } finally {
      setProcessing(false);
      closeIntentionModal();
    }
  }

  async function handleDeleteIntention(id: number) {
    setProcessing(true);
    try {
      if (connection === "local") {
        const response = await fetch(`${bridge}/api/intentions/delete`, {
          method: "POST",
          headers: { "Content-Type": "application/json" },
          body: JSON.stringify({ id }),
        });
        if (!response.ok) throw new Error("Erro ao excluir intenção.");
        const outcome = (await response.json()) as LumeOutcome;
        append("lume", outcome.message, outcome.reason);
        await refreshContext();
      } else {
        setIntentions((current) => current.filter((item) => item.id !== id));
      }
    } catch (error) {
      append("lume", "Falha ao excluir intenção.", error instanceof Error ? error.message : "Erro");
    } finally {
      setProcessing(false);
    }
  }

  async function handleUpdateIntentionStatus(id: number, status: "completed" | "dismissed") {
    setProcessing(true);
    try {
      if (connection === "local") {
        const response = await fetch(`${bridge}/api/intentions/status`, {
          method: "POST",
          headers: { "Content-Type": "application/json" },
          body: JSON.stringify({ id, status }),
        });
        if (!response.ok) throw new Error("Erro ao atualizar status da intenção.");
        const outcome = (await response.json()) as LumeOutcome;
        append("lume", outcome.message, outcome.reason);
        await refreshContext();
      } else {
        setIntentions((prev) =>
          prev.map((i) => (i.id === id ? { ...i, status } : i))
        );
      }
    } catch (err) {
      append("lume", "Falha ao atualizar intenção.", err instanceof Error ? err.message : "Erro");
    } finally {
      setProcessing(false);
    }
  }

  async function handleDeferIntention(id: number, hoursOffset: number) {
    setProcessing(true);
    try {
      const baseDate = now ?? new Date();
      const targetStart = new Date(baseDate.getTime() + hoursOffset * 3600 * 1000);
      const targetEnd = new Date(targetStart.getTime() + 2 * 3600 * 1000);
      const startIso = targetStart.toISOString();
      const endIso = targetEnd.toISOString();

      if (connection === "local") {
        const response = await fetch(`${bridge}/api/intentions/defer`, {
          method: "POST",
          headers: { "Content-Type": "application/json" },
          body: JSON.stringify({ id, start: startIso, end: endIso }),
        });
        if (!response.ok) throw new Error("Erro ao adiar intenção.");
        const outcome = (await response.json()) as LumeOutcome;
        append("lume", outcome.message, outcome.reason);
        await refreshContext();
      } else {
        setIntentions((prev) =>
          prev.map((i) => (i.id === id ? { ...i, window_start: startIso, window_end: endIso } : i))
        );
      }
    } catch (err) {
      append("lume", "Falha ao adiar intenção.", err instanceof Error ? err.message : "Erro");
    } finally {
      setProcessing(false);
    }
  }

  const [tickFeedback, setTickFeedback] = useState<string | null>(null);

  async function handleTriggerAutomation(id: number) {
    setProcessing(true);
    setTickFeedback(null);
    try {
      if (connection === "local") {
        const response = await fetch(`${bridge}/api/automations/trigger`, {
          method: "POST",
          headers: { "Content-Type": "application/json" },
          body: JSON.stringify({ id }),
        });
        if (!response.ok) throw new Error("Erro ao disparar rotina.");
        const result = (await response.json()) as LumeOutcome;
        if (result.plan_proposal) {
          setActivePlan(result.plan_proposal);
          setViewMode("plan");
          setTickFeedback(`✓ Rotina executada: Proposta de plano gerada.`);
        } else {
          setTickFeedback(`✓ ${result.message}`);
        }
        append("lume", result.message, result.reason, result.plan_proposal);
        await refreshContext();
      } else {
        setTickFeedback("Rotina simulada no modo prévia.");
        append("lume", "Rotina disparada.", "Simulação em modo prévia.");
      }
    } catch (error) {
      append("lume", "Falha ao disparar rotina.", error instanceof Error ? error.message : "Erro");
    } finally {
      setProcessing(false);
    }
  }

  async function handleTriggerTick(simulatedTime?: string) {
    setProcessing(true);
    setTickFeedback(null);
    try {
      if (connection === "local") {
        const payload: { at?: string } = {};
        if (simulatedTime) payload.at = simulatedTime;

        const response = await fetch(`${bridge}/api/tick`, {
          method: "POST",
          headers: { "Content-Type": "application/json" },
          body: JSON.stringify(payload),
        });
        if (!response.ok) throw new Error("Erro ao executar ciclo de avaliação do daemon.");
        const result = (await response.json()) as LumeOutcome;
        if (result.plan_proposal) {
          setActivePlan(result.plan_proposal);
          setViewMode("plan");
          setTickFeedback(`✓ Disparo realizado: ${result.message}`);
        } else {
          const currentTimeStr = now ? formatBlockTime(now.toISOString()) : "agora";
          setTickFeedback(
            `○ Avaliado às ${currentTimeStr}: Nenhuma rotina programada para este minuto exato. Use "▶ Disparar agora" no cartão para executar imediatamente.`
          );
        }
        append("lume", result.message, result.reason, result.plan_proposal);
        await refreshContext();
      } else {
        setTickFeedback("Ciclo executado no modo prévia.");
        append("lume", "Ciclo de avaliação executado.", "Nenhum gatilho pendente.");
      }
    } catch (error) {
      const msg = error instanceof Error ? error.message : "Erro ao avaliar gatilhos.";
      setTickFeedback(`Falha: ${msg}`);
      append("lume", "Falha ao avaliar gatilhos.", msg);
    } finally {
      setProcessing(false);
    }
  }

  async function previewAction(text: string) {
    await new Promise((resolve) => window.setTimeout(resolve, 500));
    const normalized = text.toLocaleLowerCase("pt-BR");

    if (normalized.includes("organiza") || normalized.includes("planeja")) {
      await triggerPlan(normalized.includes("semana") ? "week" : "morning");
      return;
    }

    if (normalized.includes("amanhã") && /(manhã|cedo|cedinho)/.test(normalized)) {
      const subject = extractSubject(text);
      append(
        "lume",
        "Certo. Amanhã de manhã eu trago isso de volta.",
        "Você pediu para trazer isso de volta amanhã de manhã.",
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
      "Certo. Guardei isso como contexto. Ainda não sei quando devo trazer de volta.",
      "A expressão foi preservada, mas ainda não há estrutura suficiente para o Lume agir.",
    );
  }

  async function send(rawText: string) {
    const text = rawText.trim();
    if (!text || processing) return;
    append("person", text);
    setProcessing(true);
    setStatusMessage(null);
    try {
      if (connection === "local") await localAction(text);
      else await previewAction(text);
    } catch (error) {
      if (connection === "local") {
        setConnection("preview");
        setAwaitingReply(false);
        append(
          "lume",
          "Perdi a conexão com o runtime local.",
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

  const today = now ? humanDate(now) : "Hoje";
  const welcome = now ? greeting(now) : "Olá.";

  return (
    <main className="lume-shell">
      {/* 1. Barra Superior com Estado Ambiental e Navegação Espacial */}
      <header className="topbar">
        <div className="brand-group">
          <button
            className="brand"
            type="button"
            onClick={() => {
              setViewMode("presence");
              setMessages([]);
            }}
            aria-label="Lume — início"
          >
            <span className="brand-mark" aria-hidden="true" />
            <span>lume</span>
          </button>
        </div>

        <div className="topbar-actions">
          <div className={`presence-pill ${connection}`}>
            <span className="presence-dot" aria-hidden="true" />
            <span>{connection === "local" ? "LOCAL E PRESENTE" : "PRÉVIA LOCAL"}</span>
          </div>

          <nav className="depth-nav" aria-label="Navegação por profundidade">
            <button
              className={`depth-link ${viewMode === "presence" ? "is-active" : ""}`}
              type="button"
              onClick={() => setViewMode("presence")}
            >
              Agora
            </button>
            <button
              className={`depth-link ${viewMode !== "presence" ? "is-active" : ""}`}
              type="button"
              onClick={() => setViewMode("care")}
            >
              Sob cuidado
            </button>
          </nav>
        </div>
      </header>

      {/* 2. Palco Principal Adaptativo */}
      <div className="stage-wrapper">
        {/* PROFUNDIDADE 1: PRESENÇA & DIÁLOGO */}
        {viewMode === "presence" && (
          <section className="presence-container">
            <div className="presence-hero">
              <p className="eyebrow">{today}</p>
              <h1 id="greeting">{welcome}</h1>
              <p className="opening">O que está acontecendo agora?</p>

              {attentionCandidate && attentionCandidate.is_active_now ? (
                <div className="presence-insight">
                  <p className="insight-lead">Você queria voltar a isto agora:</p>
                  <p className="insight-highlight">
                    <strong>{attentionCandidate.subject}</strong> — {shortWindow(attentionCandidate.window_start)}
                  </p>
                  {attentionCandidate.relevance_reason && (
                    <details className="attention-reason">
                      <summary>Por que agora?</summary>
                      <p>{attentionCandidate.relevance_reason}</p>
                    </details>
                  )}
                </div>
              ) : (
                <p className="presence-calm">Por enquanto, nada pede tua atenção.</p>
              )}
            </div>

            {/* Conversa ativa se houver mensagens */}
            {messages.length > 0 && (
              <div className="conversation-thread" aria-live="polite">
                {messages.map((message) => (
                  <article className={`thread-message ${message.role}`} key={message.id}>
                    <p>{message.text}</p>
                    {message.reason && (
                      <button
                        type="button"
                        className="btn-why"
                        onClick={() => setWhyOpen(whyOpen === message.id ? null : message.id)}
                      >
                        {whyOpen === message.id ? "Ocultar razão" : "Por que agora?"}
                      </button>
                    )}
                    {whyOpen === message.id && message.reason && (
                      <div className="reason-bubble">
                        <span>Razão factual</span>
                        {message.reason}
                      </div>
                    )}
                  </article>
                ))}
                {processing && (
                  <div className="thinking-indicator" aria-label="Processando...">
                    <i />
                    <i />
                    <i />
                  </div>
                )}
              </div>
            )}

            {/* Caixa de Entrada Calma */}
            <div className="composer-anchor">
              <Composer
                processing={processing}
                onSend={send}
                inputRef={composer}
              />
            </div>
          </section>
        )}

        {/* SEGUNDA PROFUNDIDADE: REVISÃO E CAPACIDADES SOB DEMANDA */}
        {viewMode === "care" && (
          <section className="care-container">
            <div className="care-header">
              <span className="section-kicker">Sob cuidado</span>
              <h2>O que você não queria perder de vista</h2>
              <p>Você pode corrigir algo aqui. Para guardar algo novo, basta dizer ao Lume.</p>
            </div>

            <div className="care-list-section">
              {openIntentions.length > 0 ? (
                <div className="intentions-list">
                  {openIntentions.map((item) => (
                    <article key={item.id} className={`intention-card ${item.status}`}>
                      <div className="intention-main">
                        <strong>{item.subject}</strong>
                        <p className="care-window">{shortWindow(item.window_start)}</p>
                      </div>

                      <div className="intention-card-actions">
                        <button type="button" className="btn-action-complete" disabled={processing} onClick={() => handleUpdateIntentionStatus(item.id, "completed")}>Resolvido</button>
                        <button type="button" className="btn-action-defer" disabled={processing} onClick={() => handleDeferIntention(item.id, 24)}>Amanhã</button>
                        <details className="item-menu">
                          <summary aria-label={`Mais opções para ${item.subject}`}>•••</summary>
                          <div>
                            <button type="button" onClick={() => openEditIntentionModal(item)}>Ajustar</button>
                            <button
                              type="button"
                              onClick={() => {
                                if (window.confirm(`Deixar de guardar “${item.subject}”?`)) void handleDeleteIntention(item.id);
                              }}
                            >
                              Deixar de guardar
                            </button>
                            <p>Interpretado localmente a partir do que você disse.</p>
                          </div>
                        </details>
                      </div>
                    </article>
                  ))}
                </div>
              ) : (
                <div className="empty-intentions-card">
                  <p>Nada em aberto.</p>
                </div>
              )}
            </div>

            <details className="more-capabilities">
              <summary>Fazer mais com este contexto</summary>
              <nav aria-label="Outras formas de cuidar do contexto">
                <button type="button" onClick={() => setViewMode("plan")}>Organizar</button>
                <button type="button" onClick={() => setViewMode("automate")}>Rotinas</button>
                <button type="button" onClick={() => setViewMode("connect")}>Conexões</button>
                <button type="button" onClick={() => setViewMode("analyze")}>Padrões</button>
              </nav>
            </details>
          </section>
        )}

        {/* PROFUNDIDADE 2: ORQUESTRAÇÃO (PROJEÇÃO DE PLANEJAMENTO) */}
        {viewMode === "plan" && (
          <section className="orchestration-container">
            <div className="orchestration-header">
              <div>
                <span className="section-kicker">Organizar</span>
                <h2>{activePlan?.summary ?? "Preparar um plano"}</h2>
              </div>
              <div className="orchestration-horizon-toggle">
                <button
                  type="button"
                  className={activePlan?.horizon === "morning" || activePlan?.horizon === "Manhã" ? "is-selected" : ""}
                  onClick={() => void triggerPlan("morning")}
                >
                  Manhã
                </button>
                <button
                  type="button"
                  className={activePlan?.horizon === "week" || activePlan?.horizon === "Semana" ? "is-selected" : ""}
                  onClick={() => void triggerPlan("week")}
                >
                  Semana
                </button>
              </div>
            </div>

            {activePlan ? (
              <div className="plan-projection-stage">
                {/* Projeção Semanal em 5 Colunas */}
                <div className="week-columns-grid">
                  {activePlan.blocks.map((block, index) => {
                    const blockDate = new Date(block.start);
                    const dayName = !Number.isNaN(blockDate.getTime())
                      ? new Intl.DateTimeFormat("pt-BR", { weekday: "short" }).format(blockDate).toUpperCase()
                      : `BLOCO ${index + 1}`;
                    return (
                      <div className={`day-column cat-${block.category}`} key={index}>
                        <div className="day-column-header">
                          <span className="day-name">{dayName}</span>
                          <span className="day-time">
                            {formatBlockTime(block.start)} - {formatBlockTime(block.end)}
                          </span>
                        </div>
                        <div className="day-concentration-bar" aria-hidden="true" />
                        <div className="day-task-card">
                          <strong className="task-title">{block.title}</strong>
                          <span className="task-category">{block.category}</span>
                        </div>
                      </div>
                    );
                  })}
                </div>

                {/* Alertas & Pontos de Atenção Factual */}
                {activePlan.points_of_attention.length > 0 && (
                  <div className="plan-attention-card">
                    <span className="attention-kicker">Pontos que merecem atenção:</span>
                    <ul>
                      {activePlan.points_of_attention.map((pt, pidx) => (
                        <li key={pidx}>{pt}</li>
                      ))}
                    </ul>
                  </div>
                )}

                {/* Barra de Consentimento Explícito */}
                <div className="plan-consent-bar">
                  {activePlan.status === "draft" ? (
                    <>
                      <button
                        type="button"
                        className="btn-apply-plan"
                        disabled={processing}
                        onClick={() => handleApplyPlan(activePlan.id)}
                      >
                        Usar este plano
                      </button>
                      <button
                        type="button"
                        className="btn-discard-plan"
                        disabled={processing}
                        onClick={() => handleDiscardPlan(activePlan.id)}
                      >
                        Descartar
                      </button>
                    </>
                  ) : activePlan.status === "applied" ? (
                    <div className="applied-pill">
                      <span>✓ Plano guardado</span>
                    </div>
                  ) : (
                    <div className="discarded-pill">
                      <span>○ Plano descartado</span>
                    </div>
                  )}
                </div>
              </div>
            ) : (
              <div className="empty-plan-prompt">
                {openIntentions.length > 0 ? (
                  <div style={{ marginBottom: "1.5rem", textAlign: "left", width: "100%", maxWidth: "560px", background: "var(--bg-subtle)", padding: "1.2rem", borderRadius: "8px", border: "1px solid var(--border-subtle)" }}>
                    <strong style={{ display: "block", marginBottom: "0.6rem", color: "var(--ink-primary)" }}>
                      {openIntentions.length === 1 ? "Uma coisa pode ser organizada:" : `${openIntentions.length} coisas podem ser organizadas:`}
                    </strong>
                    <ul style={{ paddingLeft: "1.2rem", margin: 0, fontSize: "0.9rem", color: "var(--ink-secondary)" }}>
                      {openIntentions.map((i) => (
                        <li key={i.id} style={{ marginBottom: "0.3rem" }}>
                          <strong>{i.subject}</strong> — janela: {shortWindow(i.window_start)}
                        </li>
                      ))}
                    </ul>
                  </div>
                ) : (
                  <p>Nenhum plano ativo. Clique em “Organizar minha semana” ou peça uma reorganização.</p>
                )}
                <button type="button" className="btn-primary" onClick={() => void triggerPlan("week")}>
                  Preparar um plano
                </button>
              </div>
            )}
          </section>
        )}

        {/* PROFUNDIDADE 3: EXPLORAÇÃO — AUTOMATIZAR */}
        {viewMode === "automate" && (
          <section className="exploration-container">
            <div className="exploration-header">
              <span className="section-kicker">Capacidade</span>
              <h2>Rotinas</h2>
              <p>Comportamentos que o Lume executa com permissão explícita, sem surpresas nem ações invisíveis.</p>
            </div>

            {/* Ações Rápidas de Daemon */}
            <div className="automation-actions-bar" style={{ display: "flex", flexWrap: "wrap", gap: "0.75rem", alignItems: "center" }}>
              <button
                type="button"
                className="btn-tick"
                disabled={processing}
                onClick={() => void handleTriggerTick()}
              >
                ⚡ Executar ciclo agora ({now ? formatBlockTime(now.toISOString()) : "agora"})
              </button>
              <button
                type="button"
                className="btn-tick"
                style={{ background: "rgba(255, 255, 255, 0.08)" }}
                disabled={processing}
                onClick={() => void handleTriggerTick("2026-09-22T08:30:00")}
              >
                ⏰ Testar disparo matinal (08:30)
              </button>
              <span className="tick-info">
                {automations.filter((a) => a.status === "active").length} rotina(s) ativa(s) no Ledger local.
              </span>
              {tickFeedback && (
                <div style={{ width: "100%", marginTop: "0.5rem", fontSize: "0.85rem", color: "var(--accent, #e29578)", background: "rgba(226, 149, 120, 0.1)", padding: "0.4rem 0.75rem", borderRadius: "6px" }}>
                  {tickFeedback}
                </div>
              )}
            </div>

            {/* Rotinas Ativas */}
            {automations.length > 0 ? (
              <div className="automations-grid">
                {automations.map((item) => (
                  <article key={item.id} className={`automation-card ${item.status}`}>
                    <div className="auto-header">
                      <span className={`auto-status-dot ${item.status === "active" ? "is-live" : ""}`} />
                      <strong>{item.title || "Rotina de Automação"}</strong>
                      <span className="auto-tag">#{item.id} · {item.status === "active" ? "Ativa" : "Pausada"}</span>
                    </div>
                    <div className="auto-specs">
                      <div className="spec-item">
                        <span className="spec-label">Quando:</span>
                        <span className="spec-value">{item.trigger_when}</span>
                      </div>
                      <div className="spec-item">
                        <span className="spec-label">Condição:</span>
                        <span className="spec-value">{item.condition_if || "sempre"}</span>
                      </div>
                      <div className="spec-item">
                        <span className="spec-label">Ação:</span>
                        <span className="spec-value">{item.action_then}</span>
                      </div>
                      <div className="spec-item">
                        <span className="spec-label">Permissão:</span>
                        <span className="spec-value">{humanAuthority(item.authority)}</span>
                      </div>
                    </div>
                    <div className="auto-footer">
                      <span className="last-trigger">
                        {item.last_triggered_at
                          ? `Último disparo: ${formatBlockTime(item.last_triggered_at)}`
                          : "Ainda não disparada"}
                      </span>
                      <div style={{ display: "flex", gap: "0.5rem", alignItems: "center" }}>
                        <button
                          type="button"
                          className="btn-trigger-now"
                          disabled={processing}
                          onClick={() => handleTriggerAutomation(item.id)}
                          style={{
                            padding: "0.35rem 0.75rem",
                            background: "var(--accent-ember, #c9653c)",
                            color: "#fff",
                            border: "none",
                            borderRadius: "6px",
                            fontSize: "0.8rem",
                            fontWeight: 600,
                            cursor: "pointer",
                            display: "inline-flex",
                            alignItems: "center",
                            gap: "0.3rem",
                          }}
                          title="Executar esta rotina imediatamente"
                        >
                          ▶ Disparar agora
                        </button>
                        <button
                          type="button"
                          className="btn-toggle-auto"
                          disabled={processing}
                          onClick={() => handleToggleAutomation(item.id, item.status)}
                        >
                          {item.status === "active" ? "Pausar" : "Ativar"}
                        </button>
                      </div>
                    </div>
                  </article>
                ))}
              </div>
            ) : (
              <div className="factual-empty-card">
                <span className="factual-badge">Estado Factual</span>
                <h3>Nenhuma automação personalizada foi ativada ainda.</h3>
                <p>
                  O Lume não executa comportamentos autônomos sem permissão explícita registrada no Ledger. Ative um dos modelos abaixo ou crie uma rotina via terminal com <code>lume create-automation</code>.
                </p>
              </div>
            )}

            {/* Modelos Recomendados de Rotinas */}
            <div className="templates-section">
              <h3>Modelos de Rotinas Disponíveis</h3>
              <p className="templates-desc">Ative rotinas com um clique para permitir que o Lume prepare sugestões proativas.</p>
              <div className="templates-grid">
                <article className="template-card">
                  <div className="tmpl-header">
                    <h4>Organização Matinal</h4>
                    <span className="tmpl-badge">08:30</span>
                  </div>
                  <p>Prepara uma proposta de planejamento matinal em rascunho sempre que houver intenções abertas.</p>
                  <button
                    type="button"
                    className="btn-enable-template"
                    disabled={processing}
                    onClick={() =>
                      handleCreateAutomation(
                        "Organização Matinal",
                        "08:30",
                        "has_open_intentions",
                        "propose_daily_plan",
                        "prepare_proposal",
                      )
                    }
                  >
                    + Ativar Rotina
                  </button>
                </article>

                <article className="template-card">
                  <div className="tmpl-header">
                    <h4>Fechamento do Dia</h4>
                    <span className="tmpl-badge">18:00</span>
                  </div>
                  <p>Pergunta de forma serena se as intenções do dia foram concluídas ou devem ser adiadas.</p>
                  <button
                    type="button"
                    className="btn-enable-template"
                    disabled={processing}
                    onClick={() =>
                      handleCreateAutomation(
                        "Fechamento do Dia",
                        "18:00",
                        "has_open_intentions",
                        "ask_eod_review",
                        "suggest_only",
                      )
                    }
                  >
                    + Ativar Rotina
                  </button>
                </article>

                <article className="template-card">
                  <div className="tmpl-header">
                    <h4>Aviso de Foco Iminente</h4>
                    <span className="tmpl-badge">15 min antes</span>
                  </div>
                  <p>Notifica silenciosamente antes de blocos de foco aplicados na agenda.</p>
                  <button
                    type="button"
                    className="btn-enable-template"
                    disabled={processing}
                    onClick={() =>
                      handleCreateAutomation(
                        "Aviso de Foco Iminente",
                        "15m",
                        "always",
                        "notify_block",
                        "suggest_only",
                      )
                    }
                  >
                    + Ativar Rotina
                  </button>
                </article>
              </div>
            </div>

            {/* Feed de Notificações do Daemon */}
            {notifications.length > 0 && (
              <div className="notifications-section">
                <h3>Histórico de Disparos e Notificações</h3>
                <div className="notifications-list">
                  {notifications.map((notif) => (
                    <div key={notif.id} className="notification-item">
                      <div className="notif-time">{formatBlockTime(notif.emitted_at)}</div>
                      <div className="notif-body">
                        <strong>{notif.title}</strong>
                        <p>{notif.message}</p>
                      </div>
                      {notif.action_type === "plan_proposal" && (
                        <button
                          type="button"
                          className="btn-view-notif-plan"
                          onClick={() => {
                            setViewMode("plan");
                            if (!activePlan) void triggerPlan("morning");
                          }}
                        >
                          Ver Proposta
                        </button>
                      )}
                    </div>
                  ))}
                </div>
              </div>
            )}
          </section>
        )}

        {/* PROFUNDIDADE 3: EXPLORAÇÃO — CONECTAR */}
        {viewMode === "connect" && (
          <section className="exploration-container">
            <div className="exploration-header">
              <span className="section-kicker">Capacidade</span>
              <h2>Conexões e Permissões Locais</h2>
              <p>Participantes do ecossistema e estado factual de autoridade concedida.</p>
            </div>

            <div className="connection-cards-grid">
              <article className="connection-card">
                <div className="conn-header">
                  <span className="conn-status-dot is-live" />
                  <strong>Computador local (Host)</strong>
                  <span className="conn-tag">Presente</span>
                </div>
                <p className="conn-desc">Runtime C++26 nativo, arquivos e persistência de eventos no disco local.</p>
                <div className="conn-permissions">
                  <span className="perm-ok">✓ Leitura e escrita no ledger local ({ledgerEventsCount} eventos auditáveis)</span>
                  <span className="perm-ok">✓ Execução determinística local</span>
                </div>
              </article>

              <article className="connection-card">
                <div className="conn-header">
                  <span className={`conn-status-dot ${connection === "local" ? "is-live" : ""}`} />
                  <strong>Provedor de Linguagem Local</strong>
                  <span className="conn-tag">{connection === "local" ? "Loopback isolado" : "Prévia de linguagem"}</span>
                </div>
                <p className="conn-desc">
                  {connection === "local"
                    ? "OpenAI-compatible local em 127.0.0.1. Nenhum dado sai da máquina."
                    : "Gramática determinística em execução."}
                </p>
                <div className="conn-permissions">
                  <span className="perm-ok">✓ Proposição de texto e candidatos</span>
                  <span className="perm-no">— Nenhuma autoridade de alteração direta de estado</span>
                </div>
              </article>

              <article className="connection-card is-disabled">
                <div className="conn-header">
                  <span className="conn-status-dot" />
                  <strong>Calendário Externo</strong>
                  <span className="conn-tag">Não conectado</span>
                </div>
                <p className="conn-desc">Acesso a agendas remotas desativado por padrão.</p>
                <div className="conn-permissions">
                  <span className="perm-no">— Sem leitura ou sincronização remota</span>
                </div>
              </article>
            </div>

            {/* Histórico e Auditoria de Expressões Preservadas */}
            <div className="presence-intentions-section" style={{ maxWidth: "100%", marginTop: "2rem" }}>
              <div className="intentions-section-header">
                <div>
                  <span className="section-kicker" style={{ fontSize: "0.68rem", marginBottom: "0.15rem" }}>Auditoria Factual</span>
                  <h3>Histórico de Expressões Preservadas no Ledger ({expressions.length})</h3>
                </div>
                <span className="intentions-subtitle">Sequência monotônica no SQLite WAL</span>
              </div>
              {expressions.length > 0 ? (
                <div className="expressions-history-list">
                  {expressions.map((exp) => (
                    <div key={exp.id} className="history-item">
                      <span className="history-time">#{exp.id} · {formatBlockTime(exp.recorded_at)}</span>
                      <strong className="history-text">“{exp.text}”</strong>
                      <span className="epistemic-badge">{exp.epistemic_class}</span>
                    </div>
                  ))}
                </div>
              ) : (
                <div className="empty-intentions-card">
                  <p>Nenhuma expressão registrada ainda.</p>
                </div>
              )}
            </div>
          </section>
        )}

        {/* PROFUNDIDADE 3: EXPLORAÇÃO — ANALISAR */}
        {viewMode === "analyze" && (
          <section className="exploration-container">
            <div className="exploration-header">
              <span className="section-kicker">Capacidade</span>
              <h2>Análise de Padrões e Evidências</h2>
              <p>Observações baseadas em fatos e rotinas observadas no Ledger.</p>
            </div>

            <div className="factual-empty-card">
              <span className="factual-badge">Estado Factual</span>
              <h3>Ainda não tenho observações suficientes para encontrar um padrão.</h3>
              <p>
                O Lume não inventa métricas nem dashboards decorativos. À medida que intenções, adiamentos e confirmações reais forem acumulados no Ledger ao longo das semanas, esta superfície apresentará sínteses contextuais apoiadas em evidências concretas e amostras verificáveis.
              </p>
            </div>
          </section>
        )}
      </div>

      {/* 3. Rodapé Permanente */}
      <footer className="footer-status-bar">
        <div className="footer-left">
          <span>Seu contexto fica com você.</span>
          {statusMessage && <span className="footer-status-tag">{statusMessage}</span>}
        </div>
        <div className="footer-right">
          <span className={`runtime-indicator ${connection}`}>
            {connection === "local" ? "● LOCAL E PRESENTE" : "○ Modo prévia local"}
          </span>
        </div>
      </footer>

      {/* Ajuste explícito de algo mantido em contexto */}
      {isNewModalOpen && (
        <div className="lume-modal-overlay">
          <div className="lume-modal-card" role="dialog" aria-modal="true" aria-labelledby="intention-modal-title">
            <div className="lume-modal-header">
              <h3 id="intention-modal-title">Ajustar contexto</h3>
              <button
                type="button"
                className="btn-action-dismiss"
                onClick={closeIntentionModal}
              >
                ✕
              </button>
            </div>
            <form
              className="modal-form"
              onSubmit={(e) => {
                e.preventDefault();
                if (editingIntentionId !== null) {
                  void handleEditIntention(editingIntentionId, newSubject, newStart, newEnd);
                }
              }}
            >
              <div className="form-field">
                <label htmlFor="modal-subject">O que você quer lembrar?</label>
                <input
                  id="modal-subject"
                  type="text"
                  required
                  placeholder="Ex: Pagar a conta de luz, Escrever relatório..."
                  value={newSubject}
                  onChange={(e) => setNewSubject(e.target.value)}
                />
              </div>
              <div className="form-field">
                <label htmlFor="modal-start">A partir de</label>
                <input
                  id="modal-start"
                  type="datetime-local"
                  value={newStart}
                  onChange={(e) => setNewStart(e.target.value)}
                />
              </div>
              <div className="form-field">
                <label htmlFor="modal-end">Até</label>
                <input
                  id="modal-end"
                  type="datetime-local"
                  value={newEnd}
                  onChange={(e) => setNewEnd(e.target.value)}
                />
              </div>
              <div className="modal-actions">
                <button
                  type="button"
                  className="btn-discard-plan"
                  onClick={closeIntentionModal}
                >
                  Cancelar
                </button>
                <button
                  type="submit"
                  className="btn-apply-plan"
                  disabled={
                    !newSubject.trim() ||
                    processing ||
                    !newStart ||
                    !newEnd
                  }
                >
                  Salvar
                </button>
              </div>
            </form>
          </div>
        </div>
      )}
    </main>
  );
}

function Composer({
  processing,
  onSend,
  inputRef,
}: {
  processing: boolean;
  onSend: (text: string) => Promise<void>;
  inputRef?: React.RefObject<HTMLInputElement | null>;
}) {
  const localRef = useRef<HTMLInputElement | null>(null);
  const inputEl = inputRef ?? localRef;

  const handleSubmit = (event?: FormEvent) => {
    event?.preventDefault();
    if (processing) return;
    const el = inputEl.current;
    if (!el) return;
    const trimmed = el.value.trim();
    if (!trimmed) return;
    el.value = "";
    void onSend(trimmed);
  };

  const handleKeyDown = (event: KeyboardEvent<HTMLInputElement>) => {
    if (event.key === "Enter" && !event.shiftKey) {
      event.preventDefault();
      handleSubmit();
    }
  };

  return (
    <form className="calm-composer" onSubmit={handleSubmit}>
      <label className="sr-only" htmlFor="calm-thought">
        Diga o que mudou ou peça um plano
      </label>
      <input
        ref={inputEl}
        type="text"
        id="calm-thought"
        defaultValue=""
        onKeyDown={handleKeyDown}
        placeholder="Pode falar do teu jeito, pedir um plano ou guardar um contexto…"
        disabled={processing}
        spellCheck={false}
        autoComplete="off"
        autoCorrect="off"
        autoCapitalize="off"
      />
      <button type="submit" aria-label="Enviar" disabled={processing}>
        <span aria-hidden="true">↑</span>
      </button>
    </form>
  );
}
