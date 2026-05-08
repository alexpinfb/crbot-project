package main

import (
	"bytes"
	"context"
	"crypto/tls"
	"encoding/json"
	"fmt"
	"io"
	"net"
	"net/http"
	"os"
	"strconv"
	"strings"
	"sync/atomic"
	"time"

	"github.com/gorilla/websocket"
	"github.com/joho/godotenv"
	"github.com/redis/go-redis/v9"
)

var (
	ctx      = context.Background()
	cookie   string
	provider string
	workerID string
	instance string
	redisURL string

	rdb *redis.Client

	wsLive atomic.Bool
	taking atomic.Bool

	cachedCatching atomic.Bool
	cachedSettings atomic.Value
	cachedWorkerCfg atomic.Value

	sendClient *http.Client
	crClient   *http.Client
)

type Settings struct {
	Catching         bool     `json:"catching"`
	Min             float64  `json:"min"`
	Max             float64  `json:"max"`
	Blacklist       bool     `json:"blacklistEnabled"`
	BlockBrands      []string `json:"blockBrands"`
}

type WorkerCfg struct {
	Min     float64 `json:"min"`
	Max     float64 `json:"max"`
	Enabled bool   `json:"enabled"`
}

type TakeResult struct {
	Domain string
	Status int
	Body   string
	Err    error
	Ms     int64
}

func logf(format string, args ...any) {
	fmt.Printf("%s %s\n", time.Now().UTC().Format("2006-01-02T15:04:05.000Z"), fmt.Sprintf(format, args...))
}

func getenv(k, def string) string {
	v := os.Getenv(k)
	if v == "" {
		return def
	}
	return v
}

func redisConnect() {
	opt, err := redis.ParseURL(redisURL)
	if err != nil {
		panic(err)
	}
	rdb = redis.NewClient(opt)
	if err := rdb.Ping(ctx).Err(); err != nil {
		panic(err)
	}
	logf("REDIS_READY %s instance=%s worker=%s", redisURL, instance, workerID)
}

func getSettings() Settings {
	s := Settings{Catching: true, Min: 500, Max: 50000, Blacklist: false}
	raw, err := rdb.Get(ctx, "crbot:settings").Result()
	if err != nil {
		return s
	}
	_ = json.Unmarshal([]byte(raw), &s)
	return s
}

func getSharedCatching() bool {
	v, err := rdb.Get(ctx, "crbot:catching").Result()
	if err != nil {
		logf("CATCHING_READ_FAIL error=%s", err.Error())
		return false
	}
	return v == "1" || v == "true"
}

func getWorkerCfg() WorkerCfg {
	cfg := WorkerCfg{Min: 500, Max: 50000, Enabled: true}
	raw, err := rdb.Get(ctx, "crbot:worker:"+workerID).Result()
	if err != nil {
		return cfg
	}
	_ = json.Unmarshal([]byte(raw), &cfg)
	if cfg.Max == 0 {
		cfg.Max = 50000
	}
	return cfg
}

func setStatus() {
	body, _ := json.Marshal(map[string]any{
		"workerId": workerID,
		"instance": instance,
		"ts":       time.Now().UnixMilli(),
		"ws1":      wsLive.Load(),
		"ws2":      nil,
	})
	_ = rdb.Set(ctx, "crbot:worker_status:"+workerID, string(body), 30*time.Second).Err()
}

func acquireLock(id string, amount float64, label string) bool {
	active, _ := rdb.Get(ctx, "crbot:activeOrder").Result()
	if active != "" {
		return false
	}

	body, _ := json.Marshal(map[string]any{
		"id":       id,
		"amount":   amount,
		"label":    label,
		"instance": instance,
		"ts":       time.Now().UnixMilli(),
	})

	ok, err := rdb.SetNX(ctx, "crbot:takeLock", string(body), 4*time.Second).Result()
	if err != nil {
		return false
	}
	return ok
}

func releaseLock() {
	_ = rdb.Del(ctx, "crbot:takeLock").Err()
}

func getQuoted(s, key string) string {
	pat := `"` + key + `":"`
	i := strings.Index(s, pat)
	if i < 0 {
		return ""
	}
	start := i + len(pat)
	end := strings.IndexByte(s[start:], '"')
	if end < 0 {
		return ""
	}
	return s[start : start+end]
}

func getNum(s, key string) float64 {
	q := getQuoted(s, key)
	if q != "" {
		v, _ := strconv.ParseFloat(q, 64)
		return v
	}

	pat := `"` + key + `":`
	i := strings.Index(s, pat)
	if i < 0 {
		return 0
	}
	start := i + len(pat)
	end := start
	for end < len(s) {
		c := s[end]
		if (c >= '0' && c <= '9') || c == '.' {
			end++
			continue
		}
		break
	}
	v, _ := strconv.ParseFloat(s[start:end], 64)
	return v
}

func makeHTTPClient() *http.Client {
	tr := &http.Transport{
		Proxy:               http.ProxyFromEnvironment,
		MaxIdleConns:        32,
		MaxIdleConnsPerHost: 16,
		IdleConnTimeout:     30 * time.Second,
		DisableCompression:  true,
		ForceAttemptHTTP2:   false,
		DialContext: (&net.Dialer{
			Timeout:   1200 * time.Millisecond,
			KeepAlive: 30 * time.Second,
		}).DialContext,
		TLSHandshakeTimeout: 1200 * time.Millisecond,
		TLSClientConfig:    &tls.Config{InsecureSkipVerify: true},
	}
	return &http.Client{Transport: tr, Timeout: 5 * time.Second}
}

func takeDomain(domain, id string, started time.Time, ch chan<- TakeResult) {
	client := sendClient
	if domain == "app.cr.bot" {
		client = crClient
	}

	url := "https://" + domain + "/internal/v1/p2c/payments/take/" + id
	req, err := http.NewRequest("POST", url, nil)
	if err != nil {
		ch <- TakeResult{Domain: domain, Err: err, Ms: time.Since(started).Milliseconds()}
		return
	}

	req.Header.Set("Cookie", cookie)
	req.Header.Set("Origin", "https://"+domain)
	req.Header.Set("Referer", "https://"+domain+"/")
	req.Header.Set("User-Agent", "Mozilla/5.0")
	req.Header.Set("Connection", "keep-alive")

	resp, err := client.Do(req)
	ms := time.Since(started).Milliseconds()
	if err != nil {
		ch <- TakeResult{Domain: domain, Err: err, Ms: ms}
		return
	}
	defer resp.Body.Close()

	b, _ := io.ReadAll(io.LimitReader(resp.Body, 512))
	ch <- TakeResult{Domain: domain, Status: resp.StatusCode, Body: string(b), Ms: ms}
}

func takeFast(id string, amount float64, label string) {
	if !getSharedCatching() {
		logf("TAKE_SKIP_STOP id=%s amount=%.2f via=%s reason=crbot:catching", id, amount, label)
		return
	}

	settingsNow := getSettings()
	if !settingsNow.Catching {
		logf("TAKE_SKIP_STOP id=%s amount=%.2f via=%s reason=crbot:settings", id, amount, label)
		return
	}

	lockStart := time.Now()
	if !acquireLock(id, amount, label) {
		owner, _ := rdb.Get(ctx, "crbot:takeLock").Result()
		ttl, _ := rdb.TTL(ctx, "crbot:takeLock").Result()
		logf("TAKE_SKIP_LOCK id=%s amount=%.2f via=%s owner=%s ttl=%s", id, amount, label, owner, ttl.String())
		return
	}
	lockMs := time.Since(lockStart).Milliseconds()

	started := time.Now()
	logf("TAKE_LOCK_OK id=%s amount=%.2f lockMs=%d via=%s", id, amount, lockMs, label)
	logf("TAKE_START id=%s amount=%.2f via=%s startTs=%d", id, amount, label, started.UnixMilli())
	logf("TAKE_SEND_DUAL id=%s ts=%d via=%s", id, time.Now().UnixMilli(), label)

	ch := make(chan TakeResult, 2)
	go takeDomain("app.send.tg", id, started, ch)
	go takeDomain("app.cr.bot", id, started, ch)

	r1 := <-ch
	if r1.Err != nil {
		logf("TAKE_FIRST_ERR domain=%s id=%s amount=%.2f elapsed=%dms via=%s error=%s", r1.Domain, id, amount, r1.Ms, label, r1.Err.Error())
	} else {
		logf("TAKE_FIRST_RESULT domain=%s id=%s amount=%.2f elapsed=%dms status=%d via=%s", r1.Domain, id, amount, r1.Ms, r1.Status, label)
	}

	r2 := <-ch

	ok := false
	for _, r := range []TakeResult{r1, r2} {
		if r.Err != nil {
			logf("TAKE_ERR domain=%s id=%s amount=%.2f elapsed=%dms via=%s error=%s", r.Domain, id, amount, r.Ms, label, r.Err.Error())
			continue
		}
		if r.Status == 200 {
			ok = true
			logf("TAKE_OK domain=%s id=%s amount=%.2f elapsed=%dms via=%s body=%s", r.Domain, id, amount, r.Ms, label, r.Body)
		} else {
			logf("TAKE_FAIL domain=%s id=%s amount=%.2f elapsed=%dms status=%d via=%s body=%s", r.Domain, id, amount, r.Ms, r.Status, label, r.Body)
		}
	}

	if !ok {
		releaseLock()
	}
}

func handlePacket(text string, c *websocket.Conn, label string) {
	if text == "2" {
		_ = c.WriteMessage(websocket.TextMessage, []byte("3"))
		return
	}
	if strings.HasPrefix(text, "0") {
		_ = c.WriteMessage(websocket.TextMessage, []byte("40"))
		return
	}
	if strings.HasPrefix(text, "40") {
		_ = c.WriteMessage(websocket.TextMessage, []byte(`42["list:initialize"]`))
		logf("%s_READY", label)
		return
	}

	if !strings.Contains(text, `"list:update"`) || !strings.Contains(text, `"op":"add"`) {
		return
	}

	if !cachedCatching.Load() {
		return
	}

	settings, ok := cachedSettings.Load().(Settings)
	if !ok {
		settings = Settings{Catching: true, Blacklist: false}
	}
	if !settings.Catching {
		return
	}

	id := getQuoted(text, "id")
	if id == "" {
		return
	}

	p := getQuoted(text, "provider")
	if p != provider {
		return
	}

	amount := getNum(text, "in_amount")
	cfg, ok := cachedWorkerCfg.Load().(WorkerCfg)
	if !ok || cfg.Max == 0 {
		cfg = WorkerCfg{Min: 500, Max: 50000, Enabled: true}
	}
	if amount == 0 || !cfg.Enabled || amount < cfg.Min || amount > cfg.Max {
		return
	}

	brand := strings.ToLower(getQuoted(text, "brand_name"))
	if settings.Blacklist {
		for _, x := range settings.BlockBrands {
			if x != "" && strings.Contains(brand, strings.ToLower(x)) {
				logf("SKIP_BRAND_SYNC amount=%.2f brand=%s", amount, brand)
				return
			}
		}
	}

	logf("WS_EVENT id=%s ts=%d amount=%.2f brand=%s via=%s", id, time.Now().UnixMilli(), amount, brand, label)
	takeFast(id, amount, label)
}

func wsLoop(label string) {
	headers := http.Header{}
	headers.Set("Cookie", cookie)
	headers.Set("Origin", "https://app.send.tg")
	headers.Set("User-Agent", "Mozilla/5.0")

	dialer := websocket.Dialer{
		HandshakeTimeout: 2 * time.Second,
		TLSClientConfig: &tls.Config{InsecureSkipVerify: true},
		NetDialContext: (&net.Dialer{
			Timeout:   1500 * time.Millisecond,
			KeepAlive: 30 * time.Second,
		}).DialContext,
	}

	url := "wss://app.send.tg/internal/v1/p2c-socket/?EIO=4&transport=websocket"

	for {
		logf("%s_CONNECTING", label)
		c, _, err := dialer.Dial(url, headers)
		if err != nil {
			logf("%s_ERROR %s", label, err.Error())
			wsLive.Store(false)
			time.Sleep(2 * time.Second)
			continue
		}

		wsLive.Store(true)
		logf("%s_OPEN", label)

		for {
			_, msg, err := c.ReadMessage()
			if err != nil {
				logf("%s_CLOSE error=%s", label, err.Error())
				wsLive.Store(false)
				_ = c.Close()
				break
			}
			handlePacket(string(msg), c, label)
		}

		time.Sleep(2 * time.Second)
	}
}

func warmupLoop() {
	for {
		for _, domain := range []string{"app.send.tg", "app.cr.bot"} {
			client := sendClient
			if domain == "app.cr.bot" {
				client = crClient
			}

			req, _ := http.NewRequest("POST", "https://"+domain+"/internal/v1/p2c/payments/take/warmup_probe", bytes.NewReader(nil))
			req.Header.Set("Cookie", cookie)
			req.Header.Set("Origin", "https://"+domain)
			req.Header.Set("Referer", "https://"+domain+"/")
			req.Header.Set("User-Agent", "Mozilla/5.0")

			resp, err := client.Do(req)
			if err == nil && resp.Body != nil {
				io.Copy(io.Discard, resp.Body)
				resp.Body.Close()
			}
		}
		time.Sleep(2 * time.Second)
	}
}

func settingsCacheLoop() {
	for {
		cachedCatching.Store(getSharedCatching())
		cachedSettings.Store(getSettings())
		cachedWorkerCfg.Store(getWorkerCfg())
		time.Sleep(1 * time.Second)
	}
}

func statusLoop() {
	for {
		setStatus()
		time.Sleep(5 * time.Second)
	}
}

func main() {
	_ = godotenv.Load()

	cookie = os.Getenv("COOKIE")
	if cookie == "" {
		panic("missing COOKIE")
	}

	provider = getenv("PROVIDER_ONLY", "nspk")
	workerID = getenv("WORKER_ID", "v2go")
	instance, _ = os.Hostname()
	redisURL = getenv("REDIS_URL", "redis://127.0.0.1:6379")

	sendClient = makeHTTPClient()
	crClient = makeHTTPClient()

	redisConnect()

	logf("BOT_GO_START WORKER_ID=%s PROVIDER=%s", workerID, provider)

	cachedCatching.Store(getSharedCatching())
	cachedSettings.Store(getSettings())
	cachedWorkerCfg.Store(getWorkerCfg())

	go settingsCacheLoop()
	go statusLoop()
	go warmupLoop()

	wsLoop("WS1")
}
