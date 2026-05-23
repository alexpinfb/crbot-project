package main

import (
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
	userAgent string
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
	sendIPClients map[string]*http.Client
	crIPClients   map[string]*http.Client
	baseHeadersSend http.Header
	baseHeadersCR   http.Header
	takeURLPrefixSend string
	takeURLPrefixCR   string

	sendTGIPs = []string{"138.249.21.1", "138.249.21.3"}
	crBotIPs  = []string{"138.249.21.1", "138.249.21.3"}
	dialRR    atomic.Uint64
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

func accountID() string {
	if v := getenv("ACCOUNT_ID", ""); v != "" {
		return v
	}
	if i := strings.Index(workerID, "w"); i > 0 {
		return workerID[:i]
	}
	return workerID
}

func applyAuthHeaders() {
	if baseHeadersSend != nil {
		baseHeadersSend.Set("Cookie", cookie)
		baseHeadersSend.Set("User-Agent", userAgent)
	}
	if baseHeadersCR != nil {
		baseHeadersCR.Set("Cookie", cookie)
		baseHeadersCR.Set("User-Agent", userAgent)
	}
}

func refreshAuthFromRedis() {
	if rdb == nil || workerID == "" {
		return
	}

	acc := accountID()
	ck, ckErr := rdb.Get(ctx, "crbot:account:"+acc+":cookie").Result()
	ua, uaErr := rdb.Get(ctx, "crbot:account:"+acc+":userAgent").Result()

	changed := false

	if ckErr == nil && ck != "" && ck != cookie {
		cookie = ck
		changed = true
	}

	if uaErr == nil && ua != "" && ua != userAgent {
		userAgent = ua
		changed = true
	}

	if changed {
		applyAuthHeaders()
		logf("AUTH_REDIS_UPDATED account=%s cookieLen=%d uaLen=%d", acc, len(cookie), len(userAgent))
	}
}

func authRefreshLoop() {
	for {
		refreshAuthFromRedis()
		time.Sleep(3 * time.Second)
	}
}

func registerWorkerOnce() {
	if rdb == nil || workerID == "" {
		return
	}

	acc := accountID()
	data := map[string]any{
		"workerId":  workerID,
		"worker_id": workerID,
		"accountId": acc,
		"account_id": acc,
		"instance":  instance,
		"provider":  provider,
		"online":    true,
		"updated":   time.Now().UnixMilli(),
	}

	b, err := json.Marshal(data)
	if err != nil {
		logf("WORKER_REGISTER_FAIL worker=%s error=json:%s", workerID, err.Error())
		return
	}

	if err := rdb.Set(ctx, "crbot:workerInfo:"+workerID, string(b), 15*time.Second).Err(); err != nil {
		logf("WORKER_REGISTER_FAIL worker=%s error=redis:%s", workerID, err.Error())
		return
	}

	_ = rdb.SAdd(ctx, "crbot:workers", workerID).Err()
}

func workerRegisterLoop() {
	for {
		registerWorkerOnce()
		time.Sleep(5 * time.Second)
	}
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
		return false
	}
	return v == "1" || v == "true"
}

func getWorkerCfg() WorkerCfg {
	settings := getSettings()

	cfg := WorkerCfg{
		Min: settings.Min,
		Max: settings.Max,
		Enabled: settings.Catching,
	}

	raw, err := rdb.Get(ctx, "crbot:worker:"+workerID).Result()
	if err == nil && raw != "" {
		_ = json.Unmarshal([]byte(raw), &cfg)
	}

	if cfg.Min == 0 {
		cfg.Min = settings.Min
	}

	if cfg.Max == 0 {
		cfg.Max = settings.Max
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


func accountPrefix() string {
	if len(workerID) >= 2 {
		return workerID[:2]
	}
	return workerID
}

func takeLockKey() string {
	return "crbot:takeLock:" + accountPrefix()
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
		"worker":   workerID,
		"account":  accountPrefix(),
		"ts":       time.Now().UnixMilli(),
	})

	ok, err := rdb.SetNX(ctx, takeLockKey(), string(body), 1500*time.Millisecond).Result()
	if err != nil {
		return false
	}
	return ok
}

func releaseLock() {
	_ = rdb.Del(ctx, takeLockKey()).Err()
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

func pinnedDialContext(ctx context.Context, network, address string) (net.Conn, error) {
	host, port, err := net.SplitHostPort(address)
	if err != nil {
		return dialOne(ctx, network, address)
	}

	var ips []string
	switch host {
	case "app.send.tg":
		ips = sendTGIPs
	case "app.cr.bot":
		ips = crBotIPs
	default:
		return dialOne(ctx, network, address)
	}

	type dialResult struct {
		conn   net.Conn
		err    error
		target string
	}

	raceCtx, cancel := context.WithCancel(ctx)
	defer cancel()

	ch := make(chan dialResult, len(ips))

	for _, ip := range ips {
		target := net.JoinHostPort(ip, port)
		go func(t string) {
			conn, err := dialOne(raceCtx, network, t)
			ch <- dialResult{conn: conn, err: err, target: t}
		}(target)
	}

	var lastErr error
	for i := 0; i < len(ips); i++ {
		r := <-ch
		if r.err == nil && r.conn != nil {
			logf("DIAL_RACE_WIN host=%s target=%s", host, r.target)
			cancel()
			return r.conn, nil
		}
		lastErr = r.err
	}

	if lastErr != nil {
		return nil, lastErr
	}
	return nil, context.DeadlineExceeded
}


func dialOne(ctx context.Context, network, target string) (net.Conn, error) {
	return (&net.Dialer{
		Timeout:   300 * time.Millisecond,
		KeepAlive: 60 * time.Second,
	}).DialContext(ctx, network, target)
}


func makeHTTPClient() *http.Client {
	tr := &http.Transport{
		Proxy:                 http.ProxyFromEnvironment,
		MaxIdleConns:          512,
		MaxIdleConnsPerHost:   256,
		MaxConnsPerHost:       0,
		IdleConnTimeout:       120 * time.Second,
		DisableCompression:    true,
		ForceAttemptHTTP2:     false,
		ExpectContinueTimeout: 0,
		ResponseHeaderTimeout: 2500 * time.Millisecond,
		DialContext: pinnedDialContext,
		TLSHandshakeTimeout: 500 * time.Millisecond,
		TLSClientConfig: &tls.Config{
			InsecureSkipVerify: true,
			ClientSessionCache: tls.NewLRUClientSessionCache(128),
		},
	}
	return &http.Client{Transport: tr, Timeout: 3 * time.Second}
}


func makeHTTPClientPinnedIP(ip string) *http.Client {
	tr := &http.Transport{
		Proxy:                 http.ProxyFromEnvironment,
		MaxIdleConns:          128,
		MaxIdleConnsPerHost:   64,
		MaxConnsPerHost:       0,
		IdleConnTimeout:       120 * time.Second,
		DisableCompression:    true,
		ForceAttemptHTTP2:     false,
		ExpectContinueTimeout: 0,
		ResponseHeaderTimeout: 2500 * time.Millisecond,
		DialContext: func(ctx context.Context, network, address string) (net.Conn, error) {
			_, port, err := net.SplitHostPort(address)
			if err != nil {
				return dialOne(ctx, network, address)
			}
			return dialOne(ctx, network, net.JoinHostPort(ip, port))
		},
		TLSHandshakeTimeout: 500 * time.Millisecond,
		TLSClientConfig: &tls.Config{
			InsecureSkipVerify: true,
			ClientSessionCache: tls.NewLRUClientSessionCache(128),
		},
	}
	return &http.Client{Transport: tr, Timeout: 3 * time.Second}
}

func initIPRaceClients() {
	sendIPClients = map[string]*http.Client{}
	crIPClients = map[string]*http.Client{}

	for _, ip := range sendTGIPs {
		sendIPClients[ip] = makeHTTPClientPinnedIP(ip)
	}
	for _, ip := range crBotIPs {
		crIPClients[ip] = makeHTTPClientPinnedIP(ip)
	}
}

func warmupIPRaceLoop() {
	for {
		for ip, client := range sendIPClients {
			go func(ip string, client *http.Client) {
				req, _ := http.NewRequest("POST", takeURLPrefixSend+"warmup_probe", nil)
				req.Header = baseHeadersSend
				resp, err := client.Do(req)
				if err == nil && resp.Body != nil {
					io.Copy(io.Discard, resp.Body)
					resp.Body.Close()
				}
			}(ip, client)
		}

		for ip, client := range crIPClients {
			go func(ip string, client *http.Client) {
				req, _ := http.NewRequest("POST", takeURLPrefixCR+"warmup_probe", nil)
				req.Header = baseHeadersCR
				resp, err := client.Do(req)
				if err == nil && resp.Body != nil {
					io.Copy(io.Discard, resp.Body)
					resp.Body.Close()
				}
			}(ip, client)
		}

		time.Sleep(500 * time.Millisecond)
	}
}



func takeDomain(domain, id string, started time.Time, ch chan<- TakeResult) {
	headers := baseHeadersSend
	url := takeURLPrefixSend + id
	ips := sendTGIPs
	clients := sendIPClients

	if domain == "app.cr.bot" {
		headers = baseHeadersCR
		url = takeURLPrefixCR + id
		ips = crBotIPs
		clients = crIPClients
	}

	type ipResult struct {
		res TakeResult
		ip  string
	}

	ipCh := make(chan ipResult, len(ips))

	for _, ip := range ips {
		client := clients[ip]
		if client == nil {
			client = sendClient
			if domain == "app.cr.bot" {
				client = crClient
			}
		}

		go func(ip string, client *http.Client) {
			req, err := http.NewRequest("POST", url, nil)
			if err != nil {
				ipCh <- ipResult{ip: ip, res: TakeResult{Domain: domain + "@" + ip, Err: err, Ms: time.Since(started).Milliseconds()}}
				return
			}
			req.Header = headers

			resp, err := client.Do(req)
			ms := time.Since(started).Milliseconds()
			if err != nil {
				ipCh <- ipResult{ip: ip, res: TakeResult{Domain: domain + "@" + ip, Err: err, Ms: ms}}
				return
			}
			defer resp.Body.Close()

			b, _ := io.ReadAll(io.LimitReader(resp.Body, 512))
			ipCh <- ipResult{ip: ip, res: TakeResult{Domain: domain + "@" + ip, Status: resp.StatusCode, Body: string(b), Ms: ms}}
		}(ip, client)
	}

	first := <-ipCh
	logf("TAKE_IP_FIRST domain=%s ip=%s id=%s elapsed=%dms status=%d err=%v", domain, first.ip, id, first.res.Ms, first.res.Status, first.res.Err)

	ch <- first.res
}

func takeFast(id string, amount float64, label string) {
	cfg, ok := cachedWorkerCfg.Load().(WorkerCfg)
	if !ok || cfg.Max == 0 {
		cfg = getWorkerCfg()
	}
	minAmount := cfg.Min
	maxAmount := cfg.Max

	if amount < minAmount || amount > maxAmount {
		logf("SKIP_AMOUNT id=%s amount=%.2f not in range %.2f-%.2f via=%s", id, amount, minAmount, maxAmount, label)
		return
	}


	if !getSharedCatching() {
		logf("SKIP_CATCHING_DISABLED id=%s amount=%.2f via=%s", id, amount, label)
		return
	}

	lockStart := time.Now()
	if !acquireLock(id, amount, label) {
		owner, _ := rdb.Get(ctx, takeLockKey()).Result()
		ttl, _ := rdb.TTL(ctx, takeLockKey()).Result()
		logf("TAKE_SKIP_LOCK id=%s amount=%.2f via=%s owner=%s ttl=%s", id, amount, label, owner, ttl.String())
		return
	}
	lockMs := time.Since(lockStart).Milliseconds()

	started := time.Now()
	logf("TAKE_LOCK_OK id=%s amount=%.2f lockMs=%d via=%s", id, amount, lockMs, label)
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

	results := []TakeResult{r1}
	select {
	case r2 := <-ch:
		results = append(results, r2)
	case <-time.After(150 * time.Millisecond):
		logf("TAKE_SECOND_TIMEOUT id=%s amount=%.2f via=%s waitMs=150", id, amount, label)
	}

	ok = false
	for _, r := range results {
		if r.Err != nil {
			logf("TAKE_ERR domain=%s id=%s amount=%.2f elapsed=%dms via=%s error=%s", r.Domain, id, amount, r.Ms, label, r.Err.Error())
			continue
		}
		if r.Status == 403 && strings.Contains(r.Body, "MerchantPenalized") {
			// Do not disable global catching on one worker penalty.
			_ = rdb.Set(ctx, "crbot:worker:"+workerID, fmt.Sprintf(`{"min":%.0f,"max":%.0f,"enabled":false}`, minAmount, maxAmount), 0).Err()
			logf("PENALTY_STOP worker=%s domain=%s id=%s amount=%.2f range=%.0f-%.0f body=%s", workerID, r.Domain, id, amount, minAmount, maxAmount, r.Body)
		}
		if r.Status == 200 {
			ok = true
			saveActiveOrderFromTake(r.Domain, id, amount, r.Ms, r.Body, label)
			logf("TAKE_OK domain=%s id=%s amount=%.2f elapsed=%dms via=%s body=%s", r.Domain, id, amount, r.Ms, label, r.Body)
		} else {
			if r.Status == 400 && strings.Contains(r.Body, "ActiveOrderExists") {
				if recoverActiveOrder(r.Domain, id, amount, r.Ms, label) {
					ok = true
				}
			}
			logf("TAKE_FAIL domain=%s id=%s amount=%.2f elapsed=%dms status=%d via=%s body=%s", r.Domain, id, amount, r.Ms, r.Status, label, r.Body)
		}
	}

	if !ok {
		if recoverActiveOrder("app.send.tg", id, amount, time.Since(started).Milliseconds(), label) ||
			recoverActiveOrder("app.cr.bot", id, amount, time.Since(started).Milliseconds(), label) {
			ok = true
		}
	}

	if !ok {
		releaseLock()
	}
}



func recoverActiveOrder(domain string, sourceID string, amount float64, elapsedMs int64, label string) bool {
	url := "https://" + strings.Split(domain, "@")[0] + "/internal/v1/p2c/payments"

	req, err := http.NewRequest("GET", url, nil)
	if err != nil {
		logf("ACTIVE_ORDER_RECOVER_FAIL id=%s error=req:%s", sourceID, err.Error())
		return false
	}

	req.Header.Set("Cookie", cookie)
	req.Header.Set("User-Agent", userAgent)
	req.Header.Set("Accept", "application/json")

	resp, err := sendClient.Do(req)
	if err != nil {
		logf("ACTIVE_ORDER_RECOVER_FAIL id=%s error=http:%s", sourceID, err.Error())
		return false
	}
	defer resp.Body.Close()

	bb, _ := io.ReadAll(resp.Body)
	if resp.StatusCode < 200 || resp.StatusCode >= 300 {
		logf("ACTIVE_ORDER_RECOVER_FAIL id=%s status=%d body=%s", sourceID, resp.StatusCode, string(bb))
		return false
	}

	var parsed map[string]any
	if err := json.Unmarshal(bb, &parsed); err != nil {
		logf("ACTIVE_ORDER_RECOVER_FAIL id=%s error=json:%s body=%s", sourceID, err.Error(), string(bb))
		return false
	}

	arr, ok := parsed["data"].([]any)
	if !ok || len(arr) == 0 {
		logf("ACTIVE_ORDER_RECOVER_FAIL id=%s error=no_data body=%s", sourceID, string(bb))
		return false
	}

	var best map[string]any
	now := time.Now().UTC()

	for _, it := range arr {
		m, ok := it.(map[string]any)
		if !ok {
			continue
		}

		st, _ := m["status"].(string)
		isUnlocked, _ := m["is_unlocked"].(bool)
		if st != "processing" && !(st == "completed" && !isUnlocked) {
			continue
		}

		amtStr, _ := m["amount_fiat"].(string)
		amt, _ := strconv.ParseFloat(amtStr, 64)
		diff := amt - amount
		if diff < 0 {
			diff = -diff
		}
		if diff > 0.01 {
			continue
		}

		ts, _ := m["processing_at"].(string)
		if ts == "" {
			ts, _ = m["completed_at"].(string)
		}
		t, err := time.Parse(time.RFC3339, ts)
		if err != nil {
			continue
		}
		age := now.Sub(t.UTC())
		if age < 0 || age > 2*time.Minute {
			continue
		}

		best = m
		break
	}

	if best == nil {
		logf("ACTIVE_ORDER_RECOVER_FAIL id=%s amount=%.2f error=no_fresh_processing_or_completed_order", sourceID, amount)
		return false
	}

	best["source_id"] = sourceID
	best["source_amount"] = amount
	best["domain"] = domain
	best["label"] = label
	best["instance"] = instance
	best["worker_id"] = workerID
	best["workerId"] = workerID
	best["account"] = workerID
	best["account_id"] = workerID
	best["accountId"] = workerID
	best["complete_cookie"] = cookie
	best["complete_user_agent"] = userAgent
	best["elapsed_ms"] = elapsedMs
	best["elapsedMs"] = elapsedMs
	best["saved_at_ms"] = time.Now().UnixMilli()
	best["recovered"] = true

	out, _ := json.Marshal(best)
	if err := rdb.Set(ctx, "crbot:activeOrder", string(out), 30*time.Second).Err(); err != nil {
		logf("ACTIVE_ORDER_RECOVER_FAIL id=%s error=redis:%s", sourceID, err.Error())
		return false
	}

	logf("ACTIVE_ORDER_RECOVER_OK id=%s activeId=%v status=%v amount=%v processing_at=%v body=%s", sourceID, best["id"], best["status"], best["amount_fiat"], best["processing_at"], string(out))
	return true
}


func saveActiveOrderFromTake(domain string, sourceID string, amount float64, elapsedMs int64, body string, label string) {
	var parsed map[string]any
	if err := json.Unmarshal([]byte(body), &parsed); err != nil {
		logf("ACTIVE_ORDER_SAVE_FAIL id=%s error=json:%s body=%s", sourceID, err.Error(), body)
		return
	}

	data, ok := parsed["data"].(map[string]any)
	if !ok {
		logf("ACTIVE_ORDER_SAVE_FAIL id=%s error=no_data body=%s", sourceID, body)
		return
	}

	data["source_id"] = sourceID
	data["source_amount"] = amount
	data["domain"] = domain
	data["label"] = label
	data["instance"] = instance

	data["worker_id"] = workerID
	data["workerId"] = workerID
	data["account"] = workerID
	data["account_id"] = workerID
	data["accountId"] = workerID
	data["complete_cookie"] = cookie
	data["complete_user_agent"] = userAgent

	data["elapsed_ms"] = elapsedMs
	data["elapsedMs"] = elapsedMs

	data["saved_at_ms"] = time.Now().UnixMilli()

	b, _ := json.Marshal(data)
	if err := rdb.Set(ctx, "crbot:activeOrder", string(b), 30*time.Second).Err(); err != nil {
		logf("ACTIVE_ORDER_SAVE_FAIL id=%s error=redis:%s", sourceID, err.Error())
		return
	}

	logf("ACTIVE_ORDER_SAVE_OK id=%s domain=%s elapsed=%dms body=%s", sourceID, domain, elapsedMs, string(b))
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
		cfg = WorkerCfg{Min: 2000, Max: 150000, Enabled: true}
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
		HandshakeTimeout: 700 * time.Millisecond,
		TLSClientConfig: &tls.Config{
			InsecureSkipVerify: true,
			ClientSessionCache: tls.NewLRUClientSessionCache(128),
		},
		NetDialContext: pinnedDialContext,
	}

	url := "wss://app.send.tg/internal/v1/p2c-socket/?EIO=4&transport=websocket"

	for {
		logf("%s_CONNECTING", label)
		c, _, err := dialer.Dial(url, headers)
		if err != nil {
			logf("%s_ERROR %s", label, err.Error())
			wsLive.Store(false)
			time.Sleep(3 * time.Second)
			continue
		}

		wsLive.Store(true)
		logf("%s_OPEN", label)

		_ = c.SetReadDeadline(time.Now().Add(30 * time.Second))

		c.SetPongHandler(func(string) error {
			_ = c.SetReadDeadline(time.Now().Add(30 * time.Second))
			return nil
		})

		go func(conn *websocket.Conn) {
			t := time.NewTicker(25 * time.Second)
			defer t.Stop()

			for range t.C {
				_ = conn.SetWriteDeadline(time.Now().Add(5 * time.Second))

				if err := conn.WriteMessage(websocket.PingMessage, nil); err != nil {
					logf("%s_PING_FAIL %s", label, err.Error())
					_ = conn.Close()
					return
				}
			}
		}(c)

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

		time.Sleep(3 * time.Second)
	}
}

func warmupLoop() {
	for {
		for _, domain := range []string{"app.send.tg", "app.cr.bot"} {
			client := sendClient
			if domain == "app.cr.bot" {
				client = crClient
			}

			req, _ := http.NewRequest("POST", "https://"+domain+"/internal/v1/p2c/payments/take/warmup_"+strconv.FormatInt(time.Now().UnixNano(), 10), nil)
			if domain == "app.cr.bot" {
				req.Header = baseHeadersCR
			} else {
				req.Header = baseHeadersSend
			}

			resp, err := client.Do(req)
			if err == nil && resp.Body != nil {
				io.Copy(io.Discard, resp.Body)
				resp.Body.Close()
			}
		}
		time.Sleep(15 * time.Second)
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
	envFile := os.Getenv("ENV_FILE")
	if envFile == "" {
		envFile = ".env"
	}

	_ = godotenv.Load(envFile)

	cookie = getenv("COOKIE", "dummy")
	userAgent = getenv("USER_AGENT", "Mozilla/5.0")

	provider = getenv("PROVIDER_ONLY", "nspk")
	workerID = getenv("WORKER_ID", "v2go")
	instance, _ = os.Hostname()
	redisURL = getenv("REDIS_URL", "redis://127.0.0.1:6379")

	sendClient = makeHTTPClient()
	crClient = makeHTTPClient()
	initIPRaceClients()
	initIPRaceClients()

	baseHeadersSend = http.Header{}
	baseHeadersSend.Set("Cookie", cookie)
	baseHeadersSend.Set("Origin", "https://app.send.tg")
	baseHeadersSend.Set("Referer", "https://app.send.tg/")
	baseHeadersSend.Set("User-Agent", userAgent)
	baseHeadersSend.Set("Connection", "keep-alive")

	baseHeadersCR = http.Header{}
	baseHeadersCR.Set("Cookie", cookie)
	baseHeadersCR.Set("Origin", "https://app.cr.bot")
	baseHeadersCR.Set("Referer", "https://app.cr.bot/")
	baseHeadersCR.Set("User-Agent", userAgent)
	baseHeadersCR.Set("Connection", "keep-alive")
	applyAuthHeaders()

	takeURLPrefixSend = "https://app.send.tg/internal/v1/p2c/payments/take/"
	takeURLPrefixCR = "https://app.cr.bot/internal/v1/p2c/payments/take/"

	redisConnect()
	refreshAuthFromRedis()

	logf("BOT_GO_START WORKER_ID=%s PROVIDER=%s", workerID, provider)

	cachedCatching.Store(getSharedCatching())
	cachedSettings.Store(getSettings())
	cachedWorkerCfg.Store(getWorkerCfg())

	go authRefreshLoop()
	go workerRegisterLoop()
	go settingsCacheLoop()
	go statusLoop()
	go warmupLoop()
	go warmupIPRaceLoop()

	wsLoop("WS1")
}
