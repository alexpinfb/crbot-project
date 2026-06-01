require("dotenv").config();

const fs = require("fs");
const TelegramBot = require("node-telegram-bot-api");
const WebSocket = require("ws");
const QRCode = require("qrcode");
const { exec } = require("child_process");
const { Agent, request } = require("undici");
const { createClient } = require("redis");
const os = require("os");

// ── ENV ──────────────────────────────────────────────────────────────
const BOT_TOKEN = process.env.BOT_TOKEN;
const CHAT_ID   = String(process.env.CHAT_ID);
const CHAT_IDS  = String(process.env.CHAT_IDS || process.env.CHAT_ID || "")
  .split(",")
  .map(x => x.trim())
  .filter(Boolean);
function isAllowedChat(id) {
  return CHAT_IDS.includes(String(id));
}
const COOKIE    = process.env.COOKIE;
const METHOD    = process.env.METHOD_ALFA;
const PROVIDER  = process.env.PROVIDER_ONLY || "nspk";
const TEST_MODE = process.env.TEST_MODE === "1";
const RECONNECT_MS = Number(process.env.RECONNECT_MS || 2000);

const REDIS_URL = process.env.REDIS_URL || "redis://127.0.0.1:6379";
const INSTANCE = process.env.INSTANCE || os.hostname();
const WORKER_ID = process.env.WORKER_ID || INSTANCE;

let redisReady = false;
const redis = createClient({ url: REDIS_URL });

redis.on("error", (e) => log(`REDIS_ERROR ${e.message}`));

async function connectRedis() {
  try {
    await redis.connect();
    redisReady = true;
    log(`REDIS_READY ${REDIS_URL} instance=${INSTANCE}`);
    await syncSettingsToRedis();
  } catch (e) {
    redisReady = false;
    log(`REDIS_CONNECT_FAIL ${e.message}`);
  }
}

async function getSharedActive() {
  if (!redisReady) return null;
  try { return await redis.get("crbot:activeOrder"); } catch { return null; }
}

async function setSharedActive(data) {
  if (!redisReady) return;
  try {
    log(`REDIS_ACTIVE_SET id=${data.id} amount=${data.in_amount || data.amount || "?"}`);
    await redis.set("crbot:activeOrder", JSON.stringify(data));
  } catch (e) {
    log(`REDIS_SET_ACTIVE_ERR ${e.message}`);
  }
}

async function setSharedCatching(value) {
  if (!redisReady) return;

  try {
    await redis.set("crbot:catching", value ? "1" : "0");

    await redis.publish("crbot:event", JSON.stringify({
      type: "catching",
      value
    }));

    log(`REDIS_CATCHING_SET ${value ? "ON" : "OFF"}`);

  } catch (e) {
    log(`REDIS_CATCHING_ERR ${e.message}`);
  }
}


async function syncSettingsToRedis() {
  if (!redisReady) return;

  try {
    const payload = {
      catching,
      min: MIN,
      max: MAX,
      blacklistEnabled,
      blockBrands: BLOCK_BRANDS,
      updated: Date.now(),
      instance: INSTANCE
    };

    await redis.set("crbot:settings", JSON.stringify(payload));

    log(
      `REDIS_SETTINGS_SYNC catching=${catching ? 1 : 0} min=${MIN} max=${MAX} blacklist=${blacklistEnabled ? 1 : 0} brands=${BLOCK_BRANDS.length}`
    );
  } catch (e) {
    log(`REDIS_SETTINGS_ERR ${e.message}`);
  }
}



async function getWorkerConfig() {
  const fallback = { min: MIN, max: MAX, enabled: true };
  if (!redisReady) return fallback;

  try {
    const raw = await redis.get(`crbot:worker:${WORKER_ID}`);
    if (!raw) return fallback;

    const cfg = JSON.parse(raw);
    return {
      min: typeof cfg.min === "number" ? cfg.min : fallback.min,
      max: typeof cfg.max === "number" ? cfg.max : fallback.max,
      enabled: cfg.enabled !== false
    };
  } catch {
    return fallback;
  }
}

async function setWorkerStatus(extra = {}) {
  if (!redisReady) return;
  try {
    await redis.set(`crbot:worker_status:${WORKER_ID}`, JSON.stringify({
      workerId: WORKER_ID,
      instance: INSTANCE,
      ts: Date.now(),
      ws1: ws1 && ws1.readyState === 1,
      ws2: ws2 && ws2.readyState === 1,
      ...extra
    }), { EX: 30 });
  } catch {}
}

async function getSharedCatching() {
  if (!redisReady) return catching;
  try {
    const v = await redis.get("crbot:catching");
    if (v === null) return catching;
    return v === "1";
  } catch {
    return catching;
  }
}

async function clearSharedActive() {
  if (!redisReady) return;
  try {
    log("REDIS_ACTIVE_CLEAR");
    await redis.del("crbot:activeOrder");  } catch (e) {
    log(`REDIS_CLEAR_ERR ${e.message}`);
  }
}

async function acquireTakeLock(id, amount, label) {
  if (!redisReady) return true;

  const active = await getSharedActive();
  if (active) {
    log(`TAKE_SKIP_SHARED_ACTIVE id=${id} via=${label}`);
    return false;
  }

  const ok = await redis.set(
    "crbot:takeLock",
    JSON.stringify({ id, amount, label, instance: INSTANCE, ts: Date.now() }),
    { NX: true, EX: 4 }
  );

  return ok === "OK";
}

async function releaseTakeLock() {
  if (!redisReady) return;
  try { await redis.del("crbot:takeLock", "crbot:takeLock:a1", "crbot:takeLock:a2"); } catch {}
}


if (!BOT_TOKEN || !CHAT_ID || !COOKIE) {
  console.error("Missing BOT_TOKEN / CHAT_ID / COOKIE");
  process.exit(1);
}

// ── STATE (MIN/MAX сохраняется между рестартами) ─────────────────────
let MIN = Number(process.env.MIN_AMOUNT || 300);
let MAX = Number(process.env.MAX_AMOUNT || 50000);
const STATE_FILE = "/opt/crbot/.state.json";
try {
  const s = JSON.parse(fs.readFileSync(STATE_FILE, "utf8"));
  if (s.MIN) MIN = s.MIN;
  if (s.MAX) MAX = s.MAX;
  if (typeof s.blacklistEnabled === "boolean") blacklistEnabled = s.blacklistEnabled;
  if (Array.isArray(s.BLOCK_BRANDS)) BLOCK_BRANDS = s.BLOCK_BRANDS;
  console.log(`STATE_LOADED MIN=${MIN} MAX=${MAX}`);
} catch {}

function saveState() {
  try { fs.writeFileSync(STATE_FILE, JSON.stringify({ MIN, MAX, blacklistEnabled, BLOCK_BRANDS })); } catch {}
}

// ── BLOCK LIST ───────────────────────────────────────────────────────
let blacklistEnabled = true;

let BLOCK_BRANDS = [
  "funpay", "фанпей",
  "donation", "donationalerts", "donationalert",
  "donate", "boosty", "stream", "стрим", "донат"
];

// ── UNDICI KEEP-ALIVE POOL ───────────────────────────────────────────
const sendDispatcher = new Agent({
  connections: 4,
  keepAliveTimeout: 10_000,
  keepAliveMaxTimeout: 30_000,
  connect: { rejectUnauthorized: false }
});

const crDispatcher = new Agent({
  connections: 4,
  keepAliveTimeout: 10_000,
  keepAliveMaxTimeout: 30_000,
  connect: { rejectUnauthorized: false }
});

function getDispatcherForDomain(domain) {
  return domain === "app.cr.bot" ? crDispatcher : sendDispatcher;
}

const dispatcher = sendDispatcher;

const BASE_HEADERS = {
  Cookie: COOKIE,
  Origin: "https://app.send.tg",
  Referer: "https://app.send.tg/",
  "User-Agent": "Mozilla/5.0"
};

// ── STATE FLAGS ───────────────────────────────────────────────────────
let catching    = true;
setTimeout(async () => {
  catching = true;
  await setSharedCatching(true);
  await syncSettingsToRedis();
  log("FORCE_CATCHING_ON");
}, 3000);

let shuttingDown = false;
let taking      = false;
let activeOrder = null;
let inputMode   = null;
let rangeWorker = null;
let cookieAccount = null;
let pendingNewAccount = null;
let ws1 = null, ws2 = null;
const recentIds = new Set();

// ── TELEGRAM ──────────────────────────────────────────────────────────
const tg = new TelegramBot(BOT_TOKEN, { polling: true });

const keyboard = {
  reply_markup: {
    keyboard: [
      ["🍪 Куки"],
      ["📊 Статус"],
      ["▶️ Старт", "⏸ Стоп"],
      ["🖥 Workers"]
    ],
    resize_keyboard: true
  }
};

function log(s) {
  console.log(new Date().toISOString(), s);
}

// ── WARMUP: прогреваем именно /take/ эндпоинт ──────────────────────
async function preConnect() {
  if (taking) return;
  try {
    const { body } = await request(
      "https://app.send.tg/internal/v1/p2c/payments/take/warmup_probe",
      { method: "POST", headers: BASE_HEADERS, dispatcher,
        signal: AbortSignal.timeout(4000) }
    );
    body.dump();
  } catch {}
}

// ── WS PARSER ────────────────────────────────────────────────────────
function getStr(s, key) {
  const i = s.indexOf(`"${key}":"`);
  if (i === -1) return null;
  const start = i + key.length + 4;
  const end = s.indexOf('"', start);
  return end === -1 ? null : s.slice(start, end);
}

async function handlePacket(text, sock, label, workerId) {
  if (text === "2") { if (sock.readyState === 1) sock.send("3"); return; }
  if (text.startsWith("0")) { if (sock.readyState === 1) sock.send("40"); return; }
  if (text.startsWith("40")) {
    if (sock.readyState === 1) sock.send('42["list:initialize"]');
    log(`${label}_READY`);
    return;
  }

  if (!catching || shuttingDown) return;
  if (!text.includes('"list:update"')) return;
  if (!text.includes('"op":"add"')) return;

  const id = getStr(text, "id");
  if (!id) return;

  if (recentIds.has(id)) return;
  recentIds.add(id);
  setTimeout(() => recentIds.delete(id), 10000);

  const provider = getStr(text, "provider");
  if (provider !== PROVIDER) return;

  const amount = Number(getStr(text, "in_amount"));
  const wc = await getWorkerRange(workerId);
  if (!amount || !wc.enabled || amount < wc.min || amount > wc.max) return;

  const brand = (getStr(text, "brand_name") || "").toLowerCase();
  if (blacklistEnabled && BLOCK_BRANDS.some(x => brand.includes(x))) {
    log(`SKIP_BRAND amount=${amount} brand=${brand}`);
    return;
  }

  if (TEST_MODE) { log(`TEST amount=${amount} id=${id} via=${label}`); return; }

  takeFast(id, amount, label);
}

// ── TAKE ─────────────────────────────────────────────────────────────
async function takeOneDomain(domain, id, label, started) {
  try {
    const { statusCode, body } = await request(
      `https://${domain}/internal/v1/p2c/payments/take/${id}`,
      {
        method: "POST",
        headers: {
          ...BASE_HEADERS,
          Origin: `https://${domain}`,
          Referer: `https://${domain}/`
        },
        dispatcher: getDispatcherForDomain(domain)
      }
    );

    const txt = await body.text();
    return { domain, statusCode, txt, elapsed: Date.now() - started };
  } catch (e) {
    return { domain, error: e.message, elapsed: Date.now() - started };
  }
}

async function takeFast(id, amount, label) {
  log(`NODE_TAKE_DISABLED id=${id} amount=${amount} via=${label}`);
  return;

  if (shuttingDown || !catching) return;

  const lockStarted = Date.now();
  const gotLock = await acquireTakeLock(id, amount, label);
  const lockMs = Date.now() - lockStarted;
  if (!gotLock) return;

  const catchingSnap = catching;
  const started = Date.now();
  log(`TAKE_LOCK_OK id=${id} amount=${amount} lockMs=${lockMs} via=${label}`);

  log(`TAKE_START id=${id} amount=${amount} via=${label}`);
  log(`TAKE_SEND_DUAL id=${id} ts=${Date.now()} via=${label}`);

  try {
    const p1 = takeOneDomain("app.send.tg", id, label, started);
    const p2 = takeOneDomain("app.cr.bot", id, label, started);

    const first = await Promise.race([p1, p2]);

    if (first.error) {
      log(`TAKE_FIRST_ERR domain=${first.domain} id=${id} amount=${amount} elapsed=${first.elapsed}ms via=${label} error=${first.error}`);
    } else {
      log(`TAKE_FIRST_RESULT domain=${first.domain} id=${id} amount=${amount} elapsed=${first.elapsed}ms status=${first.statusCode} via=${label}`);
    }

    const results = await Promise.all([p1, p2]);
    const okResult = results.find(r => r.statusCode === 200);

    for (const r of results) {
      if (r.error) {
        log(`TAKE_ERR domain=${r.domain} id=${id} amount=${amount} elapsed=${r.elapsed}ms via=${label} error=${r.error}`);
      } else if (r.statusCode !== 200) {
        log(`TAKE_FAIL domain=${r.domain} id=${id} amount=${amount} elapsed=${r.elapsed}ms status=${r.statusCode} via=${label} body=${r.txt.slice(0, 200)}`);
      }
    }

    if (okResult) {
      const data = JSON.parse(okResult.txt).data;
      activeOrder = data;
      data.workerId = WORKER_ID;
      data.provider = PROVIDER;
      data.instance = INSTANCE;
      data.accountName = process.env.ACCOUNT_NAME || "unknown";
      await setSharedActive(data);
      catching = false;
      await setSharedCatching(false);
      await syncSettingsToRedis();
      log(`TAKE_OK domain=${okResult.domain} id=${data.id} amount=${data.in_amount} elapsed=${okResult.elapsed}ms via=${label}`);
      sendOrderToTelegram(data, okResult.elapsed);
      return;
    }

    await releaseTakeLock();
    catching = catchingSnap;

  } catch (e) {
    await releaseTakeLock();
    catching = catchingSnap;
    log(`TAKE_FATAL amount=${amount} elapsed=${Date.now() - started}ms error=${e.message}`);
  }
}

// ── SEND ORDER TO TG ─────────────────────────────────────────────────
async function sendOrderToTelegram(data, elapsed) {
  const worker =
  data.workerId ||
  data.worker_id ||
  data.source_worker ||
  "unknown";

const elapsedMs =
  data.elapsed_ms ||
  elapsed ||
  0;

const via =
  data.via ||
  data.source_ws ||
  "WS1";

const winnerDomain =
  data.winner_domain ||
  data.source_domain ||
  "unknown";

const text =
`✅ Ордер взят

👷 Аккаунт: ${data.accountName || "unknown"}
👷 Worker: ${worker}
⚡ ${elapsedMs} ms (${via})
🌐 ${winnerDomain}

ID: ${data.id}
Сумма: ${data.in_amount} RUB
Магазин: ${data.brand_name}
QR:
${data.url}`;

  const reply_markup = {
    inline_keyboard: [
      ...(orderUrl ? [[{ text: "🔗 Открыть QR", url: orderUrl }]] : []),
      [{ text: "📋 Активные заявки", url: "https://app.send.tg/p2c/payments?tab=active" }],
      [{ text: "✅ Подтвердить", callback_data: `complete:${data.id}` }],
      [{ text: "🔓 Unlock", callback_data: "unlock" }]
    ]
  };

  try {
    if (!orderUrl) {
      log(`QR_MISSING_RAW ${JSON.stringify(data).slice(0, 1200)}`);
      throw new Error("missing order url");
    }

    const qrBuffer = await QRCode.toBuffer(orderUrl, {
      type: "png",
      width: 900,
      margin: 2
    });

    tg.sendPhoto(CHAT_ID, qrBuffer, {
      caption: text,
      reply_markup
    });
  } catch (e) {
    log(`QR_SEND_ERR ${e.message}`);
    tg.sendMessage(CHAT_ID, text, { reply_markup });
  }
}

// ── DUAL WEBSOCKET ────────────────────────────────────────────────────
function createWS(label) {
  if (shuttingDown) return null;
  log(`${label}_CONNECTING`);

  const sock = new WebSocket(
    "wss://app.send.tg/internal/v1/p2c-socket/?EIO=4&transport=websocket",
    { headers: { Cookie: COOKIE, Origin: "https://app.send.tg", "User-Agent": "Mozilla/5.0" } }
  );

  let openTime = null;

  sock.on("open", () => {
    openTime = Date.now();
    log(`${label}_OPEN`);
  });

  const workerId = label === "WS1" ? "v1" : "v2";

  sock.on("message", (buf) =>
    handlePacket(buf.toString(), sock, label, workerId)
  );

  sock.on("pong", () => {});

  // Ping каждые 10 сек чтобы сервер не закрыл соединение
  const pingTimer = setInterval(() => {
    if (sock.readyState === 1) sock.ping();
    else clearInterval(pingTimer);
  }, 10000);

  sock.on("close", (code) => {
    clearInterval(pingTimer);
    const lived = openTime ? Math.round((Date.now() - openTime) / 1000) : "?";
    log(`${label}_CLOSE code=${code} lived=${lived}s`);
    if (!shuttingDown) {
      setTimeout(() => {
        if (label === "WS1") {
          ws1 = createWS("WS1");
        }
      }, RECONNECT_MS);
    }
  });

  sock.on("error", (e) => log(`${label}_ERROR ${e.message}`));
  return sock;
}

function connectWS() {
  log("ADMIN_MODE_NO_WS");
}

// ── TG CALLBACKS ──────────────────────────────────────────────────────
tg.on("callback_query", async (q) => {
  if (!isAllowedChat(q.message?.chat?.id)) {
    console.log(new Date().toISOString(), "CHAT_DEBUG unauthorized_callback", {
      chat_id: q.message?.chat?.id,
      from_id: q.from?.id,
      username: q.from?.username,
      data: q.data
    });
    try { await tg.answerCallbackQuery(q.id, { text: "Нет доступа", show_alert: true }); } catch {}
    return;
  }
  try {
    if (q.data === "workers_refresh") {
      const txt = await getWorkerStatusesText();

      try {
        await tg.editMessageText(
          `🖥 Workers\n\n${txt}`,
          {
            chat_id: q.message.chat.id,
            message_id: q.message.message_id,
            reply_markup: {
            inline_keyboard: [
              [
                { text: "🔄 Refresh", callback_data: "workers_refresh" }
              ],
              [
                { text: "▶️ a1w1", callback_data: "worker_start:a1w1" },
                { text: "⏸ a1w1", callback_data: "worker_stop:a1w1" }
              ],
              [
                { text: "▶️ a2w1", callback_data: "worker_start:a2w1" },
                { text: "⏸ a2w1", callback_data: "worker_stop:a2w1" }
              ]
            ]
          }
        }
        );
      } catch (e) {
        if (!String(e.message).includes("message is not modified")) {
          throw e;
        }
      }

      return;
    }

    if (q.data.startsWith("worker_start:")) {
      const worker = q.data.split(":")[1];

      try {
        await redis.set(
          `crbot:worker:${worker}`,
          JSON.stringify({
            min: 300,
            max: 50000,
            enabled: true,
            updated: Date.now()
          })
        );

        await tg.answerCallbackQuery(q.id, {
          text: `${worker} started`
        });
      } catch (e) {
        await tg.answerCallbackQuery(q.id, {
          text: e.message,
          show_alert: true
        });
      }

      return;
    }

    if (q.data.startsWith("worker_stop:")) {
      const worker = q.data.split(":")[1];

      try {
        const cur = await getWorkerRange(worker);

        await redis.set(
          `crbot:worker:${worker}`,
          JSON.stringify({
            min: cur?.min ?? 300,
            max: cur?.max ?? 50000,
            enabled: false,
            updated: Date.now()
          })
        );

        await tg.answerCallbackQuery(q.id, {
          text: `${worker} stopped`
        });
      } catch (e) {
        await tg.answerCallbackQuery(q.id, {
          text: e.message,
          show_alert: true
        });
      }

      return;
    }
    if (q.data === "unlock") {
      taking = false;
      catching = true;
      await setSharedCatching(true);
      await syncSettingsToRedis();
      activeOrder = null;
      await clearSharedActive();
      await tg.answerCallbackQuery(q.id, { text: "Unlock ✅" });
      tg.sendMessage(CHAT_ID, "🟢 Catching resumed", keyboard);
      return;
    }

    if (q.data.startsWith("complete:")) {
      const [, cbId] = q.data.split(":");

      let order = activeOrder;
      try {
        const raw = await redis.get("crbot:activeOrder");
        if (raw) order = JSON.parse(raw);
      } catch {}

      const id = String(order?.id || cbId);
      const domain = String(order?.domain || "app.send.tg").split("@")[0] || "app.send.tg";
      const completeCookie = order?.complete_cookie || order?.cookie || COOKIE;
      const completeUA = order?.complete_user_agent || order?.user_agent || BASE_HEADERS["User-Agent"] || "Mozilla/5.0";

      const completeHeaders = {
        ...BASE_HEADERS,
        Cookie: completeCookie,
        Origin: `https://${domain}`,
        Referer: `https://${domain}/p2c/payments?tab=active`,
        "User-Agent": completeUA,
        "Content-Type": "application/json"
      };

      const accResp = await request(
        `https://${domain}/internal/v1/p2c/accounts`,
        {
          method: "GET",
          headers: completeHeaders,
          dispatcher,
          signal: AbortSignal.timeout(5000)
        }
      );
      const accTxt = await accResp.body.text();
      let completeMethod = null;
      try {
        const accJson = JSON.parse(accTxt);
        const accounts = Array.isArray(accJson.data) ? accJson.data : [];
        const activeAcc = accounts.find(x => x.status === "active") || accounts[0];
        completeMethod = activeAcc?.id;
      } catch {}

      if (!completeMethod) {
        await tg.answerCallbackQuery(q.id, { text: "Не найден активный счет", show_alert: true });
        tg.sendMessage(CHAT_ID, `❌ Complete fail ${id}: не найден active account. accounts=${accTxt}`, keyboard);
        return;
      }

      console.log(new Date().toISOString(), "COMPLETE_ATTEMPT", {
        id,
        cbId,
        domain,
        worker: order?.worker_id || order?.workerId,
        hasCookie: !!completeCookie,
        cookieStart: String(completeCookie || "").slice(0, 40),
        ua: completeUA
      });

      const { statusCode, body } = await request(
        `https://${domain}/internal/v1/p2c/payments/${id}/complete`,
        {
          method: "POST",
          headers: completeHeaders,
          body: JSON.stringify({ method: completeMethod }),
          dispatcher,
          signal: AbortSignal.timeout(5000)
        }
      );
      const txt = await body.text();
      if (statusCode >= 200 && statusCode < 300) {
        activeOrder = null;
        await clearSharedActive();
        await redis.del("crbot:takeLock", "crbot:takeLock:a1", "crbot:takeLock:a2");
        await tg.answerCallbackQuery(q.id, { text: "Подтверждено ✅" });
        tg.sendMessage(CHAT_ID, `✅ Ордер ${id} подтверждён через ${order?.worker_id || order?.workerId || "unknown"}`, keyboard);
      } else {
        console.log(new Date().toISOString(), "COMPLETE_RESULT", {
          id,
          statusCode,
          worker: order?.worker_id || order?.workerId,
          body: txt
        });

        if (txt && txt.includes("InvalidStatus")) {
          activeOrder = null;
          await clearSharedActive();
          await redis.del("crbot:takeLock", "crbot:takeLock:a1", "crbot:takeLock:a2");
          await tg.answerCallbackQuery(q.id, { text: "Статус уже изменился, актив снят ⚠️" });
          tg.sendMessage(CHAT_ID, `⚠️ Complete skipped ${id} / ${order?.worker_id || order?.workerId || "unknown"}: InvalidStatus, active очищен`, keyboard);
        } else {
          await tg.answerCallbackQuery(q.id, { text: txt || "Ошибка", show_alert: true });
          tg.sendMessage(CHAT_ID, `❌ Complete fail ${id} / ${order?.worker_id || order?.workerId || "unknown"}: ${txt}`, keyboard);
        }
      }
    }
  } catch (e) {
    await tg.answerCallbackQuery(q.id, { text: e.message, show_alert: true });
  }
});



async function setWorkerRange(workerId, min, max, enabled = true) {
  try {
    await redis.set(`crbot:worker:${workerId}`, JSON.stringify({
      min,
      max,
      enabled,
      updated: Date.now()
    }));
    return true;
  } catch {
    return false;
  }
}

async function getWorkerRange(workerId) {
  try {
    const raw = await redis.get(`crbot:worker:${workerId}`);
    if (!raw) return null;
    return JSON.parse(raw);
  } catch {
    return null;
  }
}

async function getRegisteredWorkerIds() {
  try {
    const ids = await redis.sMembers("crbot:workers");
    return ids.filter(Boolean).sort();
  } catch {
    return [];
  }
}

async function getWorkerInfo(workerId) {
  try {
    const raw = await redis.get(`crbot:workerInfo:${workerId}`);
    if (!raw) return null;
    return JSON.parse(raw);
  } catch {
    return null;
  }
}

function workerDisplayName(id) {
  const m = String(id).match(/^a(\d+)w(\d+)$/);
  return m ? `A${m[1]}/W${m[2]}` : String(id);
}

async function getRegisteredAccounts() {
  const ids = await getRegisteredWorkerIds();
  const set = new Set();

  for (const id of ids) {
    const info = await getWorkerInfo(id);
    const acc = info?.accountId || info?.account_id || String(id).split("w")[0];
    if (acc) set.add(acc);
  }

  return Array.from(set).sort();
}

async function getNextAccountId() {
  const accounts = await getRegisteredAccounts();
  let max = 0;

  for (const acc of accounts) {
    const m = String(acc).match(/^a(\d+)$/);
    if (m) max = Math.max(max, Number(m[1]));
  }

  return `a${max + 1}`;
}

function accountDisplayName(acc) {
  const m = String(acc).match(/^a(\d+)$/);
  return m ? `👤 Аккаунт ${m[1]}` : `👤 ${acc}`;
}

function accountFromButton(text) {
  const m = String(text).match(/^👤 Аккаунт (\d+)$/);
  if (m) return `a${m[1]}`;
  const m2 = String(text).match(/^👤\s+(a\d+)$/);
  if (m2) return m2[1];
  return null;
}

function normalizeCookie(text) {
  return String(text || "")
    .replace(/\r/g, "\n")
    .split("\n")
    .map(x => x.trim())
    .filter(Boolean)
    .join("; ")
    .replace(/;+\s*;/g, ";")
    .replace(/\s+/g, " ")
    .trim()
    .replace(/;\s*$/, "");
}

async function getWorkersForAccount(acc) {
  const ids = await getRegisteredWorkerIds();
  const out = [];

  for (const id of ids) {
    const info = await getWorkerInfo(id);
    const infoAcc = info?.accountId || info?.account_id || String(id).split("w")[0];
    if (infoAcc === acc) out.push(id);
  }

  return out.sort();
}

async function getWorkerStatusesText() {
  let ids = await getRegisteredWorkerIds();

  if (!ids.length) {
    const keys = await redis.keys("crbot:worker:*");
    ids = keys
      .filter(k => /^crbot:worker:a[1-9]w[1-9]$/.test(k))
      .map(k => k.split(":").pop())
      .sort();
  }

  const lines = [];
  const now = Date.now();

  for (const id of ids) {
    const cfg = await getWorkerRange(id);
    const info = await getWorkerInfo(id);

    const name = workerDisplayName(id);
    const enabled = cfg?.enabled ?? false;
    const icon = enabled ? "🟢" : "🔴";
    const min = cfg?.min ?? "?";
    const max = cfg?.max ?? "?";

    const updated = Number(info?.updated || 0);
    const online = updated > 0 && (now - updated) < 20000;
    const onlineIcon = online ? "📡" : "⚫";

    lines.push(`${name}: ${icon} ${min}-${max} ${onlineIcon}`);
  }

  return lines.join("\n");
}

function defaultRangeForWorker(workerId) {
  if (String(workerId).startsWith("a1")) return { min: 500, max: 150000 };
  if (String(workerId).startsWith("a2")) return { min: 160, max: 5000 };
  return { min: 300, max: 50000 };
}

async function setWorkerEnabled(workerId, enabled) {
  const cur = await getWorkerRange(workerId);
  const def = defaultRangeForWorker(workerId);
  const min = cur?.min ?? def.min;
  const max = cur?.max ?? def.max;
  return setWorkerRange(workerId, min, max, enabled);
}

async function getWorkersToggleText() {
  const names = { v1: "WS1", v2: "WS2", v3: "WS3", v4: "WS4" };
  const lines = [];
  for (const id of ["v1", "v2", "v3", "v4"]) {
    const cfg = await getWorkerRange(id);
    if (!cfg) {
      lines.push(`${names[id]}: ⚪ нет данных`);
    } else {
      lines.push(`${names[id]}: ${cfg.enabled ? "🟢 ON" : "🔴 OFF"} ${cfg.min}-${cfg.max}`);
    }
  }
  return lines.join("\n");
}



// ── GO WORKER ACTIVE ORDER WATCHER ─────────────────────────────────────
let lastGoActiveOrderKey = null;

async function goActiveOrderWatcher() {
  try {
    const raw = await redis.get("crbot:activeOrder");
    if (!raw) return;

    const order = JSON.parse(raw);
    const key = `${order.id || ""}:${order.payload || order.url || ""}`;

    if (!key || key === lastGoActiveOrderKey) return;

    lastGoActiveOrderKey = key;
    activeOrder = order;

    log(`GO_ACTIVE_ORDER_NOTIFY id=${order.id} amount=${order.in_amount || order.amount}`);
    sendOrderToTelegram(order, "worker");
  } catch (e) {
    log(`GO_ACTIVE_ORDER_WATCH_ERR ${e.message}`);
  }
}

setInterval(goActiveOrderWatcher, 500);

// ── TG MESSAGES ───────────────────────────────────────────────────────
tg.on("message", async (msg) => {
  if (!isAllowedChat(msg.chat.id)) {
    console.log(new Date().toISOString(), "CHAT_DEBUG unauthorized", {
      chat_id: msg.chat.id,
      from_id: msg.from?.id,
      username: msg.from?.username,
      first_name: msg.from?.first_name,
      text: msg.text
    });
    return;
  }
  const t = msg.text || "";

  if (t === "🖥 Workers") {
    const accounts = await getRegisteredAccounts();
    const rows = accounts.map(acc => [accountDisplayName(acc)]);
    rows.push(["➕ Аккаунт"]);
    rows.push(["↩️ Назад"]);

    const txt = await getWorkerStatusesText();

    tg.sendMessage(CHAT_ID, `🖥 Workers\n\n${txt}\n\nВыбери аккаунт:`, {
      reply_markup: {
        keyboard: rows,
        resize_keyboard: true
      }
    });
    return;
  }


  if (t.includes("Назад")) {
    inputMode = null;
    rangeWorker = null;
    tg.sendMessage(CHAT_ID, "Ок", keyboard);
    return;
  }

  if (inputMode === "blackadd") {
    const v = t.trim().toLowerCase();
    inputMode = null;
    if (v && !BLOCK_BRANDS.includes(v)) BLOCK_BRANDS.push(v);
    saveState();
    await syncSettingsToRedis();
    tg.sendMessage(CHAT_ID, `✅ Добавлено в ЧС: ${v}`, keyboard);
    return;
  }

  if (inputMode === "blackdel") {
    const v = t.trim().toLowerCase();
    inputMode = null;
    BLOCK_BRANDS = BLOCK_BRANDS.filter(x => x !== v);
    saveState();
    await syncSettingsToRedis();
    tg.sendMessage(CHAT_ID, `✅ Удалено из ЧС: ${v}`, keyboard);
    return;
  }

  if (inputMode === "new_account_cookie") {
    const cookieText = normalizeCookie(t);

    if (!pendingNewAccount) {
      inputMode = null;
      pendingNewAccount = null;
      tg.sendMessage(CHAT_ID, "Ошибка: аккаунт не выбран", keyboard);
      return;
    }

    if (!cookieText.includes("access_token=")) {
      tg.sendMessage(CHAT_ID, "Это не похоже на cookie. Вставь полную строку cookie с access_token=");
      return;
    }

    const ua = BASE_HEADERS["User-Agent"] || process.env.USER_AGENT || "Mozilla/5.0";
    const acc = pendingNewAccount;

    await redis.set(`crbot:account:${acc}:cookie`, cookieText);
    await redis.set(`crbot:account:${acc}:userAgent`, ua);
    await redis.set(`crbot:account:${acc}:created`, String(Date.now()));

    inputMode = null;
    pendingNewAccount = null;

    tg.sendMessage(
      CHAT_ID,
      `✅ Аккаунт ${acc.toUpperCase()} создан.\nТеперь на новом сервере ставь worker с WORKER_ID=${acc}w1.`,
      keyboard
    );
    return;
  }

  if (inputMode === "cookie") {
    const cookieText = normalizeCookie(t);

    if (!cookieAccount) {
      inputMode = null;
      cookieAccount = null;
      tg.sendMessage(CHAT_ID, "Ошибка: аккаунт не выбран", keyboard);
      return;
    }

    if (!cookieText.includes("access_token=")) {
      tg.sendMessage(CHAT_ID, "Это не похоже на cookie. Вставь полную строку cookie с access_token=");
      return;
    }

    const ua = BASE_HEADERS["User-Agent"] || process.env.USER_AGENT || "Mozilla/5.0";

    await redis.set(`crbot:account:${cookieAccount}:cookie`, cookieText);
    await redis.set(`crbot:account:${cookieAccount}:userAgent`, ua);

    inputMode = null;
    const doneAccount = cookieAccount;
    cookieAccount = null;

    tg.sendMessage(CHAT_ID, `✅ Cookie сохранены для ${doneAccount}. Worker подхватит за 3 секунды.`, keyboard);
    return;
  }

  if (inputMode === "range") {
    const m = t.trim().match(/^(\d+(?:\.\d+)?)\s+(\d+(?:\.\d+)?)$/);
    if (!m || !rangeWorker) {
      tg.sendMessage(CHAT_ID, "Введи диапазон так: 500 3000");
      return;
    }

    const min = Number(m[1]);
    const max = Number(m[2]);

    if (!Number.isFinite(min) || !Number.isFinite(max) || min < 0 || max <= min) {
      tg.sendMessage(CHAT_ID, "Ошибка. Пример: 500 3000");
      return;
    }

    const cur = await getWorkerRange(rangeWorker);
    const ok = await setWorkerRange(rangeWorker, min, max, cur?.enabled ?? false);
    const doneWorker = rangeWorker;
    inputMode = null;
    rangeWorker = null;

    tg.sendMessage(CHAT_ID, ok ? `✅ ${doneWorker}: ${min}-${max}` : `❌ Не смог сохранить ${doneWorker}`, keyboard);
    return;
  }


  if (t === "➕ Аккаунт") {
    pendingNewAccount = await getNextAccountId();
    inputMode = "new_account_cookie";

    tg.sendMessage(
      CHAT_ID,
      `Создаём ${pendingNewAccount.toUpperCase()}.\nВставь полный COOKIE для нового аккаунта.`
    );
    return;
  }

  const selectedAccount = accountFromButton(t);
  if (selectedAccount) {
    const workers = await getWorkersForAccount(selectedAccount);
    const rows = [
      [`▶️ Старт ${selectedAccount.toUpperCase()}`, `⏸ Стоп ${selectedAccount.toUpperCase()}`],
      ...workers.map(id => [`⚙️ Диапазон ${id}`]),
      ...workers.map(id => [`🟢🔴 ${id}`]),
      [`🍪 Куки ${selectedAccount.toUpperCase()}`],
      ["↩️ Назад"]
    ];

    tg.sendMessage(CHAT_ID, accountDisplayName(selectedAccount), {
      reply_markup: {
        keyboard: rows,
        resize_keyboard: true
      }
    });
    return;
  }

  if (t === "🍪 Куки") {
    const accounts = await getRegisteredAccounts();
    const rows = accounts.map(acc => [`🍪 Куки ${acc.toUpperCase()}`]);
    rows.push(["↩️ Назад"]);

    tg.sendMessage(CHAT_ID, "Выбери аккаунт для обновления cookie:", {
      reply_markup: {
        keyboard: rows,
        resize_keyboard: true
      }
    });
    return;
  }

  const cookieMatch = t.match(/^🍪 Куки (A\d+)$/i);
  if (cookieMatch) {
    cookieAccount = cookieMatch[1].toLowerCase();
    inputMode = "cookie";

    tg.sendMessage(
      CHAT_ID,
      `Вставь полный COOKIE для ${cookieAccount}.`
    );
    return;
  }



  if (t === "⚙️ Воркеры") {
    const accounts = await getRegisteredAccounts();
    const rows = accounts.map(acc => [accountDisplayName(acc)]);
    rows.push(["↩️ Назад"]);

    tg.sendMessage(CHAT_ID, "Выбери аккаунт:", {
      reply_markup: {
        keyboard: rows,
        resize_keyboard: true
      }
    });
    return;
  }


  const workerToggleMatch = t.trim().match(/^🟢🔴\s+(a\d+w\d+)$/i);
  if (workerToggleMatch) {
    const workerId = workerToggleMatch[1].toLowerCase();

    const cur = await getWorkerRange(workerId);
    const enabled = !(cur?.enabled ?? false);

    await setWorkerEnabled(workerId, enabled);

    const updated = await getWorkerRange(workerId);

    tg.sendMessage(
      CHAT_ID,
      `${workerId}\n${updated.enabled ? "🟢 ON" : "🔴 OFF"}\n${updated.min}-${updated.max}`
    );

    return;
  }



  if (t === "⚙️ Диапазоны") {
    const accounts = await getRegisteredAccounts();
    const rows = accounts.map(acc => [`⚙️ Диапазон ${acc.toUpperCase()}`]);
    rows.push(["↩️ Назад"]);

    tg.sendMessage(CHAT_ID, "Выбери аккаунт:", {
      reply_markup: {
        keyboard: rows,
        resize_keyboard: true
      }
    });
    return;
  }

  const rangeMatchButton = t.match(/^⚙️ Диапазон (a\d+w\d+)$/i);
  if (rangeMatchButton) {
    rangeWorker = rangeMatchButton[1].toLowerCase();
    inputMode = "range";

    const cur = await getWorkerRange(rangeWorker);
    const current = cur ? `${cur.min}-${cur.max}` : "нет данных";

    tg.sendMessage(
      CHAT_ID,
      `${t} сейчас: ${current}\nВведи новый диапазон: 500 3000`
    );
    return;
  }

  const rangeMatch = t.trim().match(/^a(\d+)w(\d+)\s+(\d+(?:\.\d+)?)\s+(\d+(?:\.\d+)?)$/i);
  if (rangeMatch) {
    const workerId = `a${rangeMatch[1]}w${rangeMatch[2]}`;
    const min = Number(rangeMatch[3]);
    const max = Number(rangeMatch[4]);

    if (!Number.isFinite(min) || !Number.isFinite(max) || min < 0 || max <= min) {
      tg.sendMessage(CHAT_ID, "Формат: a1w1 500 3000");
      return;
    }

    const cur = await getWorkerRange(workerId);
    const ok = await setWorkerRange(workerId, min, max, cur?.enabled ?? false);
    tg.sendMessage(CHAT_ID, ok ? `✅ ${workerId}: ${min}-${max}` : `❌ Не смог сохранить ${workerId}`, keyboard);
    return;
  }

  if (t.includes("Старт")) {
    catching = true;
    await setSharedCatching(true);
    await syncSettingsToRedis();
    const workersText = await getWorkerStatusesText();
    tg.sendMessage(CHAT_ID,
      `🟢 Ловля включена\n\nWS / workers:\n${workersText}`,
      keyboard);
    return;
  }
  if (t.includes("Стоп") && !t.includes("Полный")) {
    catching = false;
    await setSharedCatching(false);
    await syncSettingsToRedis();
    // WS НЕ трогаем — они живут и греются
    // После Старт — работаем на тех же живых сокетах
    tg.sendMessage(CHAT_ID,
      "⏸ Ловля на паузе\nWS живут, жми Старт когда готов",
      keyboard);
    return;
  }
  if (t.includes("Полный стоп")) {
    tg.sendMessage(CHAT_ID, "🛑 Останавливаю...");
    setTimeout(() => exec("systemctl stop crbot"), 500);
    return;
  }
  if (t.includes("Активный ордер")) {
    let order = activeOrder;

    if (!order) {
      try {
        const raw = await redis.get("crbot:activeOrder");
        if (raw) order = JSON.parse(raw);
      } catch {}
    }

    if (!order) { tg.sendMessage(CHAT_ID, "Активного ордера нет", keyboard); return; }

    activeOrder = order;
    sendOrderToTelegram(order, "повтор");
    return;
  }

  if (t.includes("ЧС ON/OFF")) {
    blacklistEnabled = !blacklistEnabled;
    saveState();
    await syncSettingsToRedis();
    tg.sendMessage(CHAT_ID, `🚫 ЧС: ${blacklistEnabled ? "ON" : "OFF"}`, keyboard);
    return;
  }

  if (t.includes("Показать ЧС")) {
    tg.sendMessage(CHAT_ID,
      `🚫 ЧС: ${blacklistEnabled ? "ON" : "OFF"}\n\n` +
      (BLOCK_BRANDS.length ? BLOCK_BRANDS.map((x, i) => `${i + 1}. ${x}`).join("\n") : "Пусто"),
      keyboard
    );
    return;
  }

  if (t.includes("Добавить в ЧС")) {
    inputMode = "blackadd";
    tg.sendMessage(CHAT_ID, "Введи слово/бренд для добавления в ЧС:");
    return;
  }

  if (t.includes("Удалить из ЧС")) {
    inputMode = "blackdel";
    tg.sendMessage(CHAT_ID, "Введи слово/бренд для удаления из ЧС:");
    return;
  }

  if (t.includes("Статус")) {
    const workersText = await getWorkerStatusesText();

    tg.sendMessage(CHAT_ID,
      `WS / workers:\n${workersText}\n\n` +
      `Catching: ${catching ? "ON" : "OFF"}\n` +
      `Taking: ${taking ? "YES" : "NO"}\n` +
      `Mode: ${TEST_MODE ? "TEST" : "LIVE"}`,
      keyboard
    );
    return;
  }
});

// ── SHUTDOWN ──────────────────────────────────────────────────────────
function shutdown(sig) {
  shuttingDown = true; catching = false;
  log(`${sig} stopping`);
  try { ws1.close(); } catch {}
  try { ws2.close(); } catch {}
  setTimeout(() => process.exit(0), 300);
}
process.on("SIGTERM", () => shutdown("SIGTERM"));
process.on("SIGINT",  () => shutdown("SIGINT"));

// ── START ─────────────────────────────────────────────────────────────
log(`BOT_START WORKER_ID=${WORKER_ID}`);
connectRedis();
// connectWS();
preConnect();
setInterval(preConnect, 2000); // держим TLS соединение живым

setInterval(() => setWorkerStatus(), 5000);
