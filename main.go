package main

import (
	"bytes"
	"context"
	"crypto/tls"
	"encoding/json"
	"fmt"
	"io"
	"log"
	"net"
	"net/http"
	"os"
	"os/signal"
	"strconv"
	"strings"
	"sync"
	"sync/atomic"
	"syscall"
	"time"

	"github.com/go-redis/redis/v8"
	tgbotapi "github.com/go-telegram-bot-api/telegram-bot-api/v5"
	"github.com/gorilla/websocket"
	"github.com/joho/godotenv"
	qrcode "github.com/skip2/go-qrcode"
)

// ── CONFIG ────────────────────────────────────────────────────────────

type Config struct {
	BotToken    string
	ChatID      int64
	Cookie      string
	Method      string
	Provider    string
	TestMode    bool
	ReconnectMs int
	RedisURL    string
	WorkerID    string
	MinAmount   float64
	MaxAmount   float64
}

func loadConfig() Config {
	_ = godotenv.Load("/opt/crbot/.env")

	chatID, _ := strconv.ParseInt(os.Getenv("CHAT_ID"), 10, 64)
	reconnMs, _ := strconv.Atoi(os.Getenv("RECONNECT_MS"))
	if reconnMs == 0 {
		reconnMs = 2000
	}
	minA, _ := strconv.ParseFloat(os.Getenv("MIN_AMOUNT"), 64)
	if minA == 0 {
		minA = 300
	}
	maxA, _ := strconv.ParseFloat(os.Getenv("MAX_AMOUNT"), 64)
	if maxA == 0 {
		maxA = 50000
	}
	provider := os.Getenv("PROVIDER_ONLY")
	if provider == "" {
		provider = "nspk"
	}
	workerID := os.Getenv("WORKER_ID")
	if workerID == "" {
		h, _ := os.Hostname()
		workerID = h
	}
	redisURL := os.Getenv("REDIS_URL")
	if redisURL == "" {
		redisURL = "redis://127.0.0.1:6379"
	}

	return Config{
		BotToken:    os.Getenv("BOT_TOKEN"),
		ChatID:      chatID,
		Cookie:      os.Getenv("COOKIE"),
		Method:      os.Getenv("METHOD_ALFA"),
		Provider:    provider,
		TestMode:    os.Getenv("TEST_MODE") == "1",
		ReconnectMs: reconnMs,
		RedisURL:    redisURL,
		WorkerID:    workerID,
		MinAmount:   minA,
		MaxAmount:   maxA,
	}
}

// ── STATE ─────────────────────────────────────────────────────────────

type State struct {
	mu               sync.RWMutex
	catching         bool
	shutting         bool
	activeOrder      *OrderData
	blacklistEnabled bool
	blockBrands      []string
	minAmount        float64
	maxAmount        float64
}

type OrderData struct {
	ID        string  `json:"id"`
	InAmount  float64 `json:"in_amount"`
	BrandName string  `json:"brand_name"`
	URL       string  `json:"url"`
}

// ── SEEN IDs dedup ────────────────────────────────────────────────────

type SeenSet struct {
	mu    sync.Mutex
	items map[string]time.Time
}

func newSeenSet() *SeenSet {
	s := &SeenSet{items: make(map[string]time.Time)}
	go s.cleanup()
	return s
}

func (s *SeenSet) Add(id string) bool {
	s.mu.Lock()
	defer s.mu.Unlock()
	if _, ok := s.items[id]; ok {
		return false
	}
	s.items[id] = time.Now().Add(10 * time.Second)
	return true
}

func (s *SeenSet) cleanup() {
	for range time.Tick(5 * time.Second) {
		s.mu.Lock()
		now := time.Now()
		for k, exp := range s.items {
			if now.After(exp) {
				delete(s.items, k)
			}
		}
		s.mu.Unlock()
	}
}

// ── HTTP CLIENTS ──────────────────────────────────────────────────────

func newHTTPClient() *http.Client {
	tr := &http.Transport{
		TLSClientConfig: &tls.Config{InsecureSkipVerify: true},
		DialContext: (&net.Dialer{
			Timeout:   5 * time.Second,
			KeepAlive: 30 * time.Second,
		}).DialContext,
		MaxIdleConns:        16,
		MaxIdleConnsPerHost: 8,
		IdleConnTimeout:     30 * time.Second,
		DisableCompression:  true,
	}
	return &http.Client{Transport: tr, Timeout: 8 * time.Second}
}

var (
	sendClient = newHTTPClient()
	crClient   = newHTTPClient()
)

// ── TAKE RESULT ───────────────────────────────────────────────────────

type TakeResult struct {
	Domain     string
	StatusCode int
	Body       []byte
	Elapsed    time.Duration
	Err        error
}

func takeOneDomain(ctx context.Context, domain, id, cookie string) TakeResult {
	start := time.Now()
	url := fmt.Sprintf("https://%s/internal/v1/p2c/payments/take/%s", domain, id)

	req, err := http.NewRequestWithContext(ctx, "POST", url, nil)
	if err != nil {
		return TakeResult{Domain: domain, Err: err, Elapsed: time.Since(start)}
	}
	req.Header.Set("Cookie", cookie)
	req.Header.Set("Origin", "https://"+domain)
	req.Header.Set("Referer", "https://"+domain+"/")
	req.Header.Set("User-Agent", "Mozilla/5.0")

	client := sendClient
	if domain == "app.cr.bot" {
		client = crClient
	}

	resp, err := client.Do(req)
	if err != nil {
		return TakeResult{Domain: domain, Err: err, Elapsed: time.Since(start)}
	}
	defer resp.Body.Close()
	body, _ := io.ReadAll(resp.Body)
	return TakeResult{
		Domain:     domain,
		StatusCode: resp.StatusCode,
		Body:       body,
		Elapsed:    time.Since(start),
	}
}

// ── REDIS ─────────────────────────────────────────────────────────────

type RedisClient struct {
	client *redis.Client
	ready  atomic.Bool
	ctx    context.Context
}

func newRedisClient(url string) *RedisClient {
	opt, err := redis.ParseURL(url)
	if err != nil {
		log.Printf("REDIS_PARSE_ERR %v", err)
		return &RedisClient{ctx: context.Background()}
	}
	r := &RedisClient{
		client: redis.NewClient(opt),
		ctx:    context.Background(),
	}
	if err := r.client.Ping(r.ctx).Err(); err != nil {
		log.Printf("REDIS_CONNECT_FAIL %v", err)
	} else {
		r.ready.Store(true)
		log.Printf("REDIS_READY %s", url)
	}
	return r
}

func (r *RedisClient) AcquireLock(id string, amount float64, label string) bool {
	if !r.ready.Load() {
		return true
	}
	// Check active order
	active, _ := r.client.Get(r.ctx, "crbot:activeOrder").Result()
	if active != "" {
		log.Printf("TAKE_SKIP_SHARED_ACTIVE id=%s via=%s", id, label)
		return false
	}

	val, _ := json.Marshal(map[string]interface{}{
		"id": id, "amount": amount, "label": label, "ts": time.Now().UnixMilli(),
	})
	ok, err := r.client.SetNX(r.ctx, "crbot:takeLock", string(val), 4*time.Second).Result()
	if err != nil || !ok {
		log.Printf("TAKE_SKIP_LOCK id=%s via=%s", id, label)
		return false
	}
	return true
}

func (r *RedisClient) ReleaseLock() {
	if !r.ready.Load() {
		return
	}
	r.client.Del(r.ctx, "crbot:takeLock")
}

func (r *RedisClient) SetActive(data *OrderData, instance string) {
	if !r.ready.Load() {
		return
	}
	val, _ := json.Marshal(map[string]interface{}{
		"id": data.ID, "amount": data.InAmount, "brand": data.BrandName,
		"url": data.URL, "instance": instance, "ts": time.Now().UnixMilli(),
	})
	r.client.Set(r.ctx, "crbot:activeOrder", string(val), 0)
}

func (r *RedisClient) ClearActive() {
	if !r.ready.Load() {
		return
	}
	r.client.Del(r.ctx, "crbot:activeOrder")
	r.client.Del(r.ctx, "crbot:takeLock")
}

func (r *RedisClient) SetCatching(v bool) {
	if !r.ready.Load() {
		return
	}
	val := "0"
	if v {
		val = "1"
	}
	r.client.Set(r.ctx, "crbot:catching", val, 0)
}

func (r *RedisClient) GetCatching() *bool {
	if !r.ready.Load() {
		return nil
	}
	v, err := r.client.Get(r.ctx, "crbot:catching").Result()
	if err != nil {
		return nil
	}
	b := v == "1"
	return &b
}

func (r *RedisClient) SetWorkerStatus(workerID, instance string, ws1, ws2 bool) {
	if !r.ready.Load() {
		return
	}
	val, _ := json.Marshal(map[string]interface{}{
		"workerId": workerID, "instance": instance,
		"ts": time.Now().UnixMilli(), "ws1": ws1, "ws2": ws2,
	})
	r.client.Set(r.ctx, "crbot:worker_status:"+workerID, string(val), 30*time.Second)
}

func (r *RedisClient) GetWorkerConfig(workerID string, defMin, defMax float64) (float64, float64, bool) {
	if !r.ready.Load() {
		return defMin, defMax, true
	}
	raw, err := r.client.Get(r.ctx, "crbot:worker:"+workerID).Result()
	if err != nil {
		return defMin, defMax, true
	}
	var cfg map[string]interface{}
	if err := json.Unmarshal([]byte(raw), &cfg); err != nil {
		return defMin, defMax, true
	}
	minV := defMin
	maxV := defMax
	enabled := true
	if v, ok := cfg["min"].(float64); ok {
		minV = v
	}
	if v, ok := cfg["max"].(float64); ok {
		maxV = v
	}
	if v, ok := cfg["enabled"].(bool); ok {
		enabled = v
	}
	return minV, maxV, enabled
}

func (r *RedisClient) SetWorkerRange(workerID string, min, max float64) {
	if !r.ready.Load() {
		return
	}
	val, _ := json.Marshal(map[string]interface{}{
		"min": min, "max": max, "enabled": true, "updated": time.Now().UnixMilli(),
	})
	r.client.Set(r.ctx, "crbot:worker:"+workerID, string(val), 0)
}

// ── WS PARSER ─────────────────────────────────────────────────────────

func getStr(s, key string) string {
	needle := `"` + key + `":"`
	i := strings.Index(s, needle)
	if i == -1 {
		return ""
	}
	start := i + len(needle)
	end := strings.Index(s[start:], `"`)
	if end == -1 {
		return ""
	}
	return s[start : start+end]
}

// ── WARMUP ────────────────────────────────────────────────────────────

func warmup(cookie string) {
	req, err := http.NewRequest("POST",
		"https://app.send.tg/internal/v1/p2c/payments/take/warmup_probe", nil)
	if err != nil {
		return
	}
	req.Header.Set("Cookie", cookie)
	req.Header.Set("Origin", "https://app.send.tg")
	req.Header.Set("User-Agent", "Mozilla/5.0")
	ctx, cancel := context.WithTimeout(context.Background(), 4*time.Second)
	defer cancel()
	req = req.WithContext(ctx)
	resp, err := sendClient.Do(req)
	if err != nil {
		return
	}
	io.Copy(io.Discard, resp.Body)
	resp.Body.Close()
}

// ── SNIPER ────────────────────────────────────────────────────────────

type Sniper struct {
	cfg      Config
	state    *State
	seen     *SeenSet
	rdb      *RedisClient
	bot      *tgbotapi.BotAPI
	ws1      *websocket.Conn
	ws2      *websocket.Conn
	wsMu     sync.Mutex
	hostname string
}

func (s *Sniper) takeFast(id string, amount float64, label string) {
	s.state.mu.RLock()
	catching := s.state.catching
	shutting := s.state.shutting
	s.state.mu.RUnlock()

	if !catching || shutting {
		return
	}

	if !s.rdb.AcquireLock(id, amount, label) {
		return
	}

	started := time.Now()
	logf("TAKE_START id=%s amount=%.2f via=%s", id, amount, label)
	logf("TAKE_SEND_DUAL id=%s ts=%d via=%s", id, time.Now().UnixMilli(), label)

	ctx, cancel := context.WithTimeout(context.Background(), 6*time.Second)
	defer cancel()

	ch := make(chan TakeResult, 2)
	go func() { ch <- takeOneDomain(ctx, "app.send.tg", id, s.cfg.Cookie) }()
	go func() { ch <- takeOneDomain(ctx, "app.cr.bot", id, s.cfg.Cookie) }()

	results := make([]TakeResult, 0, 2)
	var okResult *TakeResult

	for i := 0; i < 2; i++ {
		r := <-ch
		results = append(results, r)
		if r.Err != nil {
			logf("TAKE_ERR domain=%s id=%s elapsed=%dms error=%v", r.Domain, id, r.Elapsed.Milliseconds(), r.Err)
		} else if r.StatusCode == 200 && okResult == nil {
			cp := r
			okResult = &cp
		} else {
			logf("TAKE_FAIL domain=%s id=%s amount=%.2f elapsed=%dms status=%d via=%s body=%s",
				r.Domain, id, amount, r.Elapsed.Milliseconds(), r.StatusCode, label,
				truncate(string(r.Body), 200))
		}
	}

	if okResult != nil {
		var resp struct {
			Data OrderData `json:"data"`
		}
		if err := json.Unmarshal(okResult.Body, &resp); err != nil {
			logf("TAKE_PARSE_ERR %v", err)
			s.rdb.ReleaseLock()
			return
		}
		data := resp.Data
		s.state.mu.Lock()
		s.state.catching = false
		s.state.activeOrder = &data
		s.state.mu.Unlock()

		s.rdb.SetActive(&data, s.hostname)
		s.rdb.SetCatching(false)

		logf("TAKE_OK domain=%s id=%s amount=%.2f elapsed=%dms via=%s",
			okResult.Domain, data.ID, data.InAmount, time.Since(started).Milliseconds(), label)

		go s.sendOrderToTelegram(&data, time.Since(started).Milliseconds())
		return
	}

	s.rdb.ReleaseLock()
}

func (s *Sniper) handlePacket(text string, label string) {
	if text == "2" {
		return // pong handled by ws loop
	}
	if strings.HasPrefix(text, "0") {
		return
	}
	if strings.HasPrefix(text, "40") {
		logf("%s_READY", label)
		return
	}

	s.state.mu.RLock()
	catching := s.state.catching
	shutting := s.state.shutting
	minA := s.state.minAmount
	maxA := s.state.maxAmount
	blacklistEnabled := s.state.blacklistEnabled
	blockBrands := s.state.blockBrands
	s.state.mu.RUnlock()

	if !catching || shutting {
		return
	}
	if !strings.Contains(text, `"list:update"`) {
		return
	}
	if !strings.Contains(text, `"op":"add"`) {
		return
	}

	id := getStr(text, "id")
	if id == "" {
		return
	}

	if !s.seen.Add(id) {
		return
	}

	provider := getStr(text, "provider")
	if provider != s.cfg.Provider {
		return
	}

	amountStr := getStr(text, "in_amount")
	amount, err := strconv.ParseFloat(amountStr, 64)
	if err != nil || amount == 0 {
		return
	}

	// Per-worker config from Redis
	wMin, wMax, wEnabled := s.rdb.GetWorkerConfig(s.cfg.WorkerID, minA, maxA)
	if !wEnabled || amount < wMin || amount > wMax {
		return
	}

	brand := strings.ToLower(getStr(text, "brand_name"))
	if blacklistEnabled {
		for _, b := range blockBrands {
			if strings.Contains(brand, b) {
				logf("SKIP_BRAND amount=%.2f brand=%s", amount, brand)
				return
			}
		}
	}

	if s.cfg.TestMode {
		logf("TEST amount=%.2f id=%s via=%s", amount, id, label)
		return
	}

	go s.takeFast(id, amount, label)
}

// ── WEBSOCKET ─────────────────────────────────────────────────────────

func (s *Sniper) runWS(label string, getConn func() *websocket.Conn, setConn func(*websocket.Conn)) {
	for {
		s.state.mu.RLock()
		shutting := s.state.shutting
		s.state.mu.RUnlock()
		if shutting {
			return
		}

		logf("%s_CONNECTING", label)

		dialer := websocket.Dialer{
			TLSClientConfig:  &tls.Config{InsecureSkipVerify: true},
			HandshakeTimeout: 10 * time.Second,
		}
		header := http.Header{}
		header.Set("Cookie", s.cfg.Cookie)
		header.Set("Origin", "https://app.send.tg")
		header.Set("User-Agent", "Mozilla/5.0")

		conn, _, err := dialer.Dial(
			"wss://app.send.tg/internal/v1/p2c-socket/?EIO=4&transport=websocket",
			header,
		)
		if err != nil {
			logf("%s_ERROR %v", label, err)
			time.Sleep(time.Duration(s.cfg.ReconnectMs) * time.Millisecond)
			continue
		}

		s.wsMu.Lock()
		setConn(conn)
		s.wsMu.Unlock()

		openTime := time.Now()
		logf("%s_OPEN", label)

		// Ping goroutine
		pingDone := make(chan struct{})
		go func() {
			ticker := time.NewTicker(10 * time.Second)
			defer ticker.Stop()
			for {
				select {
				case <-ticker.C:
					if err := conn.WriteMessage(websocket.PingMessage, nil); err != nil {
						return
					}
				case <-pingDone:
					return
				}
			}
		}()

		for {
			_, msg, err := conn.ReadMessage()
			if err != nil {
				break
			}
			text := string(msg)

			// EIO heartbeat
			if text == "2" {
				conn.WriteMessage(websocket.TextMessage, []byte("3"))
				continue
			}
			if strings.HasPrefix(text, "0") {
				conn.WriteMessage(websocket.TextMessage, []byte("40"))
				continue
			}
			if strings.HasPrefix(text, "40") {
				conn.WriteMessage(websocket.TextMessage, []byte(`42["list:initialize"]`))
				logf("%s_READY", label)
				continue
			}

			s.handlePacket(text, label)
		}

		close(pingDone)
		lived := time.Since(openTime).Seconds()
		logf("%s_CLOSE lived=%.0fs", label, lived)

		s.wsMu.Lock()
		setConn(nil)
		s.wsMu.Unlock()

		time.Sleep(time.Duration(s.cfg.ReconnectMs) * time.Millisecond)
	}
}

// ── TELEGRAM ──────────────────────────────────────────────────────────

func (s *Sniper) sendOrderToTelegram(data *OrderData, elapsedMs int64) {
	text := fmt.Sprintf(
		"✅ Ордер взят\n\nID: %s\nСумма: %.2f RUB\nМагазин: %s\nСкорость: %d ms\n\nQR:\n%s",
		data.ID, data.InAmount, data.BrandName, elapsedMs, data.URL,
	)

	markup := tgbotapi.InlineKeyboardMarkup{
		InlineKeyboard: [][]tgbotapi.InlineKeyboardButton{
			{tgbotapi.NewInlineKeyboardButtonURL("🔗 Открыть QR", data.URL)},
			{tgbotapi.NewInlineKeyboardButtonURL("📋 Активные заявки", "https://app.send.tg/p2c/payments?tab=active")},
			{tgbotapi.NewInlineKeyboardButtonData("✅ Подтвердить Альфа", "complete:"+data.ID+":alfa")},
			{tgbotapi.NewInlineKeyboardButtonData("🔓 Unlock", "unlock")},
		},
	}

	// Generate QR
	qrBytes, err := qrcode.Encode(data.URL, qrcode.Medium, 512)
	if err == nil {
		photo := tgbotapi.NewPhoto(s.cfg.ChatID, tgbotapi.FileBytes{
			Name:  "qr.png",
			Bytes: qrBytes,
		})
		photo.Caption = text
		photo.ReplyMarkup = markup
		if _, err := s.bot.Send(photo); err != nil {
			logf("QR_SEND_ERR %v", err)
			// fallback
			msg := tgbotapi.NewMessage(s.cfg.ChatID, text)
			msg.ReplyMarkup = markup
			s.bot.Send(msg)
		}
	} else {
		msg := tgbotapi.NewMessage(s.cfg.ChatID, text)
		msg.ReplyMarkup = markup
		s.bot.Send(msg)
	}
}

func (s *Sniper) mainKeyboard() tgbotapi.ReplyKeyboardMarkup {
	return tgbotapi.NewReplyKeyboard(
		tgbotapi.NewKeyboardButtonRow(
			tgbotapi.NewKeyboardButton("▶️ Старт"),
			tgbotapi.NewKeyboardButton("⏸ Стоп"),
		),
		tgbotapi.NewKeyboardButtonRow(
			tgbotapi.NewKeyboardButton("📊 Статус"),
			tgbotapi.NewKeyboardButton("📋 Активный ордер"),
		),
		tgbotapi.NewKeyboardButtonRow(
			tgbotapi.NewKeyboardButton("🛑 Полный стоп"),
		),
		tgbotapi.NewKeyboardButtonRow(
			tgbotapi.NewKeyboardButton("🚫 ЧС ON/OFF"),
			tgbotapi.NewKeyboardButton("📋 Показать ЧС"),
		),
		tgbotapi.NewKeyboardButtonRow(
			tgbotapi.NewKeyboardButton("➕ Добавить в ЧС"),
			tgbotapi.NewKeyboardButton("➖ Удалить из ЧС"),
		),
		tgbotapi.NewKeyboardButtonRow(
			tgbotapi.NewKeyboardButton("⚙️ Диапазоны"),
		),
	)
}

func (s *Sniper) sendMsg(text string) {
	msg := tgbotapi.NewMessage(s.cfg.ChatID, text)
	msg.ReplyMarkup = s.mainKeyboard()
	s.bot.Send(msg)
}

func (s *Sniper) workerStatusText() string {
	ids := []string{"v1", "v2", "v3", "v4"}
	names := map[string]string{"v1": "WS1/main", "v2": "WS2", "v3": "WS3", "v4": "WS4"}
	lines := []string{}
	for _, id := range ids {
		raw, err := s.rdb.client.Get(s.rdb.ctx, "crbot:worker_status:"+id).Result()
		if err != nil || raw == "" {
			lines = append(lines, names[id]+": ⚪ нет данных")
			continue
		}
		var st map[string]interface{}
		if err := json.Unmarshal([]byte(raw), &st); err != nil {
			lines = append(lines, names[id]+": ⚪ bad status")
			continue
		}
		ws1ok, _ := st["ws1"].(bool)
		icon := "🔴"
		if ws1ok {
			icon = "🟢"
		}
		wMin, wMax, _ := s.rdb.GetWorkerConfig(id, s.cfg.MinAmount, s.cfg.MaxAmount)
		lines = append(lines, fmt.Sprintf("%s: %s %.0f-%.0f", names[id], icon, wMin, wMax))
	}
	return strings.Join(lines, "\n")
}

type tgHandler struct {
	s          *Sniper
	inputMode  string
	rangeWorker string
}

func (h *tgHandler) handle(update tgbotapi.Update) {
	s := h.s

	// Callback queries
	if update.CallbackQuery != nil {
		q := update.CallbackQuery
		s.bot.Request(tgbotapi.NewCallback(q.ID, ""))

		if q.Data == "unlock" {
			s.state.mu.Lock()
			s.state.catching = false
			s.state.activeOrder = nil
			s.state.mu.Unlock()
			s.rdb.SetCatching(false)
			s.rdb.ClearActive()
			s.sendMsg("🔓 Unlock выполнен")
			return
		}

		if strings.HasPrefix(q.Data, "complete:") {
			parts := strings.Split(q.Data, ":")
			if len(parts) < 2 {
				return
			}
			id := parts[1]
			go func() {
				url := fmt.Sprintf("https://app.send.tg/internal/v1/p2c/payments/%s/complete", id)
				body, _ := json.Marshal(map[string]string{"method": s.cfg.Method})
				req, _ := http.NewRequest("POST", url, bytes.NewReader(body))
				req.Header.Set("Cookie", s.cfg.Cookie)
				req.Header.Set("Origin", "https://app.send.tg")
				req.Header.Set("Content-Type", "application/json")
				req.Header.Set("User-Agent", "Mozilla/5.0")
				resp, err := sendClient.Do(req)
				if err != nil {
					s.sendMsg(fmt.Sprintf("❌ Complete err: %v", err))
					return
				}
				defer resp.Body.Close()
				if resp.StatusCode >= 200 && resp.StatusCode < 300 {
					s.state.mu.Lock()
					s.state.activeOrder = nil
					s.state.mu.Unlock()
					s.rdb.ClearActive()
					s.sendMsg(fmt.Sprintf("✅ Ордер %s подтверждён", id))
				} else {
					b, _ := io.ReadAll(resp.Body)
					s.sendMsg(fmt.Sprintf("❌ Complete fail %s: %s", id, string(b)))
				}
			}()
		}
		return
	}

	// Text messages
	if update.Message == nil {
		return
	}
	if update.Message.Chat.ID != s.cfg.ChatID {
		return
	}

	t := update.Message.Text

	// Input modes
	if h.inputMode == "blackadd" {
		v := strings.ToLower(strings.TrimSpace(t))
		h.inputMode = ""
		s.state.mu.Lock()
		found := false
		for _, b := range s.state.blockBrands {
			if b == v {
				found = true
				break
			}
		}
		if !found && v != "" {
			s.state.blockBrands = append(s.state.blockBrands, v)
		}
		s.state.mu.Unlock()
		s.sendMsg(fmt.Sprintf("✅ Добавлено в ЧС: %s", v))
		return
	}

	if h.inputMode == "blackdel" {
		v := strings.ToLower(strings.TrimSpace(t))
		h.inputMode = ""
		s.state.mu.Lock()
		newList := []string{}
		for _, b := range s.state.blockBrands {
			if b != v {
				newList = append(newList, b)
			}
		}
		s.state.blockBrands = newList
		s.state.mu.Unlock()
		s.sendMsg(fmt.Sprintf("✅ Удалено из ЧС: %s", v))
		return
	}

	if h.inputMode == "range" && h.rangeWorker != "" {
		parts := strings.Fields(t)
		if len(parts) != 2 {
			s.bot.Send(tgbotapi.NewMessage(s.cfg.ChatID, "Введи: 500 3000"))
			return
		}
		minV, err1 := strconv.ParseFloat(parts[0], 64)
		maxV, err2 := strconv.ParseFloat(parts[1], 64)
		if err1 != nil || err2 != nil || minV < 0 || maxV <= minV {
			s.bot.Send(tgbotapi.NewMessage(s.cfg.ChatID, "Ошибка. Пример: 500 3000"))
			return
		}
		s.rdb.SetWorkerRange(h.rangeWorker, minV, maxV)
		worker := h.rangeWorker
		h.inputMode = ""
		h.rangeWorker = ""
		s.sendMsg(fmt.Sprintf("✅ %s: %.0f-%.0f", worker, minV, maxV))
		return
	}

	// Commands
	if strings.Contains(t, "Назад") {
		h.inputMode = ""
		h.rangeWorker = ""
		s.sendMsg("Ок")
		return
	}

	if t == "⚙️ Диапазоны" {
		kb := tgbotapi.NewReplyKeyboard(
			tgbotapi.NewKeyboardButtonRow(
				tgbotapi.NewKeyboardButton("WS1"),
				tgbotapi.NewKeyboardButton("WS2"),
			),
			tgbotapi.NewKeyboardButtonRow(
				tgbotapi.NewKeyboardButton("WS3"),
				tgbotapi.NewKeyboardButton("WS4"),
			),
			tgbotapi.NewKeyboardButtonRow(
				tgbotapi.NewKeyboardButton("↩️ Назад"),
			),
		)
		msg := tgbotapi.NewMessage(s.cfg.ChatID, "Выбери воркер:")
		msg.ReplyMarkup = kb
		s.bot.Send(msg)
		return
	}

	wsMap := map[string]string{"WS1": "v1", "WS2": "v2", "WS3": "v3", "WS4": "v4"}
	if wid, ok := wsMap[t]; ok {
		h.rangeWorker = wid
		h.inputMode = "range"
		wMin, wMax, _ := s.rdb.GetWorkerConfig(wid, s.cfg.MinAmount, s.cfg.MaxAmount)
		s.bot.Send(tgbotapi.NewMessage(s.cfg.ChatID,
			fmt.Sprintf("%s сейчас: %.0f-%.0f\nВведи новый диапазон: 500 3000", t, wMin, wMax)))
		return
	}

	if strings.Contains(t, "Старт") {
		s.state.mu.Lock()
		s.state.catching = true
		s.state.mu.Unlock()
		s.rdb.SetCatching(true)
		workers := s.workerStatusText()
		s.sendMsg("🟢 Ловля включена\n\nWS / workers:\n" + workers)
		return
	}

	if strings.Contains(t, "Стоп") && !strings.Contains(t, "Полный") {
		s.state.mu.Lock()
		s.state.catching = false
		s.state.mu.Unlock()
		s.rdb.SetCatching(false)
		s.sendMsg("⏸ Ловля на паузе\nWS живут, жми Старт когда готов")
		return
	}

	if strings.Contains(t, "Полный стоп") {
		s.sendMsg("🛑 Останавливаю...")
		time.Sleep(500 * time.Millisecond)
		syscall.Kill(syscall.Getpid(), syscall.SIGTERM)
		return
	}

	if strings.Contains(t, "Активный ордер") {
		s.state.mu.RLock()
		order := s.state.activeOrder
		s.state.mu.RUnlock()
		if order == nil {
			s.sendMsg("Активного ордера нет")
		} else {
			go s.sendOrderToTelegram(order, 0)
		}
		return
	}

	if strings.Contains(t, "ЧС ON/OFF") {
		s.state.mu.Lock()
		s.state.blacklistEnabled = !s.state.blacklistEnabled
		enabled := s.state.blacklistEnabled
		s.state.mu.Unlock()
		onoff := "OFF"
		if enabled {
			onoff = "ON"
		}
		s.sendMsg(fmt.Sprintf("🚫 ЧС: %s", onoff))
		return
	}

	if strings.Contains(t, "Показать ЧС") {
		s.state.mu.RLock()
		brands := s.state.blockBrands
		enabled := s.state.blacklistEnabled
		s.state.mu.RUnlock()
		onoff := "OFF"
		if enabled {
			onoff = "ON"
		}
		text := fmt.Sprintf("🚫 ЧС: %s\n\n", onoff)
		if len(brands) == 0 {
			text += "Пусто"
		} else {
			for i, b := range brands {
				text += fmt.Sprintf("%d. %s\n", i+1, b)
			}
		}
		s.sendMsg(text)
		return
	}

	if strings.Contains(t, "Добавить в ЧС") {
		h.inputMode = "blackadd"
		s.bot.Send(tgbotapi.NewMessage(s.cfg.ChatID, "Введи слово/бренд для добавления в ЧС:"))
		return
	}

	if strings.Contains(t, "Удалить из ЧС") {
		h.inputMode = "blackdel"
		s.bot.Send(tgbotapi.NewMessage(s.cfg.ChatID, "Введи слово/бренд для удаления из ЧС:"))
		return
	}

	if strings.Contains(t, "Статус") {
		s.state.mu.RLock()
		catching := s.state.catching
		blacklist := s.state.blacklistEnabled
		brandsCount := len(s.state.blockBrands)
		order := s.state.activeOrder
		s.state.mu.RUnlock()

		s.wsMu.Lock()
		ws1ok := s.ws1 != nil
		ws2ok := s.ws2 != nil
		s.wsMu.Unlock()

		w1 := "🔴"
		if ws1ok {
			w1 = "🟢"
		}
		w2 := "🔴"
		if ws2ok {
			w2 = "🟢"
		}

		activeID := "none"
		if order != nil {
			activeID = order.ID
		}
		workers := s.workerStatusText()

		s.sendMsg(fmt.Sprintf(
			"WS / workers:\n%s\n\nWS1: %s  WS2: %s\nCatching: %s\nBlacklist: %s (%d)\nMode: %s\nActive: %s",
			workers, w1, w2,
			boolStr(catching, "ON", "OFF"),
			boolStr(blacklist, "ON", "OFF"),
			brandsCount,
			boolStr(s.cfg.TestMode, "TEST", "LIVE"),
			activeID,
		))
		return
	}
}

// ── HELPERS ───────────────────────────────────────────────────────────

func logf(format string, args ...interface{}) {
	log.Printf(format, args...)
}

func truncate(s string, n int) string {
	if len(s) <= n {
		return s
	}
	return s[:n]
}

func boolStr(b bool, yes, no string) string {
	if b {
		return yes
	}
	return no
}

// ── MAIN ──────────────────────────────────────────────────────────────

func main() {
	cfg := loadConfig()

	if cfg.BotToken == "" || cfg.ChatID == 0 || cfg.Cookie == "" {
		log.Fatal("Missing BOT_TOKEN / CHAT_ID / COOKIE")
	}

	hostname, _ := os.Hostname()

	state := &State{
		catching:         true,
		blacklistEnabled: true,
		blockBrands: []string{
			"funpay", "фанпей",
			"donation", "donationalerts", "donationalert",
			"donate", "boosty", "stream", "стрим", "донат",
		},
		minAmount: cfg.MinAmount,
		maxAmount: cfg.MaxAmount,
	}

	rdb := newRedisClient(cfg.RedisURL)
	seen := newSeenSet()

	bot, err := tgbotapi.NewBotAPI(cfg.BotToken)
	if err != nil {
		log.Fatalf("TG_INIT_ERR %v", err)
	}
	log.Printf("BOT_START WORKER_ID=%s", cfg.WorkerID)

	sniper := &Sniper{
		cfg:      cfg,
		state:    state,
		seen:     seen,
		rdb:      rdb,
		bot:      bot,
		hostname: hostname,
	}

	// Start WS goroutines
	go sniper.runWS("WS1",
		func() *websocket.Conn { return sniper.ws1 },
		func(c *websocket.Conn) { sniper.ws1 = c },
	)
	time.Sleep(200 * time.Millisecond)
	go sniper.runWS("WS2",
		func() *websocket.Conn { return sniper.ws2 },
		func(c *websocket.Conn) { sniper.ws2 = c },
	)

	// Warmup loop
	go func() {
		for {
			warmup(cfg.Cookie)
			time.Sleep(5 * time.Second)
		}
	}()

	// Worker status heartbeat
	go func() {
		for {
			sniper.wsMu.Lock()
			ws1ok := sniper.ws1 != nil
			ws2ok := sniper.ws2 != nil
			sniper.wsMu.Unlock()
			rdb.SetWorkerStatus(cfg.WorkerID, hostname, ws1ok, ws2ok)
			time.Sleep(5 * time.Second)
		}
	}()

	// TG polling
	u := tgbotapi.NewUpdate(0)
	u.Timeout = 30
	updates := bot.GetUpdatesChan(u)
	handler := &tgHandler{s: sniper}

	go func() {
		for update := range updates {
			handler.handle(update)
		}
	}()

	// Graceful shutdown
	sig := make(chan os.Signal, 1)
	signal.Notify(sig, syscall.SIGTERM, syscall.SIGINT)
	<-sig

	log.Println("SHUTTING DOWN")
	state.mu.Lock()
	state.shutting = true
	state.catching = false
	state.mu.Unlock()

	sniper.wsMu.Lock()
	if sniper.ws1 != nil {
		sniper.ws1.Close()
	}
	if sniper.ws2 != nil {
		sniper.ws2.Close()
	}
	sniper.wsMu.Unlock()

	time.Sleep(300 * time.Millisecond)
}
