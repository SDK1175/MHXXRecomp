// The admin view (docs/hub-admin.md): an HTTP page and API on a loopback address only, opened through an SSH tunnel by
// the admin apps. It shows the hubs and who is in them (hunter name, port id, a hash of the address - never the
// address itself); players never see any of it.
//
// Locks: the tunnel (SSH keys) and a token, made on the first start in the state directory (admin.token, 0600). The
// apps read it over SSH and open /?token=..., which sets a cookie; the API also takes "Authorization: Bearer ...".
package main

import (
	"crypto/rand"
	"crypto/subtle"
	"embed"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"net"
	"net/http"
	"os"
	"path/filepath"
	"sort"
	"strings"
	"time"
	"unicode/utf8"
)

//go:embed admin
var adminFiles embed.FS

type Admin struct {
	s       *Server
	st      *store
	token   string
	started time.Time
	version string
}

// NewAdmin: the token is read from (or written to) dir/admin.token.
func NewAdmin(s *Server, dir, version string) (*Admin, error) {
	if err := os.MkdirAll(dir, 0700); err != nil {
		return nil, err
	}
	path := filepath.Join(dir, "admin.token")
	b, err := os.ReadFile(path)
	tok := strings.TrimSpace(string(b))
	if err != nil || len(tok) < 32 {
		r := make([]byte, 24)
		if _, err := rand.Read(r); err != nil {
			return nil, err
		}
		tok = hex.EncodeToString(r)
		if err := os.WriteFile(path, []byte(tok+"\n"), 0600); err != nil {
			return nil, err
		}
	}
	a := &Admin{s: s, st: openStore(dir), token: tok, started: time.Now(), version: version}
	s.mu.Lock()
	s.banned = a.st.banned
	s.netOf = a.st.netOf
	s.onRecord = a.st.record
	s.mu.Unlock()
	go a.st.janitor()
	return a, nil
}

// ListenAndServe: only on a loopback address (the page is reached through SSH, never from the internet).
func (a *Admin) ListenAndServe(addr string) error {
	host, _, err := net.SplitHostPort(addr)
	if err != nil {
		return err
	}
	if ip := net.ParseIP(host); host != "localhost" && (ip == nil || !ip.IsLoopback()) {
		return errors.New("the admin view listens on a loopback address only (e.g. 127.0.0.1:39520)")
	}
	srv := &http.Server{Addr: addr, Handler: a.Handler(), ReadHeaderTimeout: 10 * time.Second}
	return srv.ListenAndServe()
}

const (
	adminCookie  = "mhxx_admin"
	deviceCookie = "mhxx_device"
)

func cleanDevice(d string) string {
	d = strings.Map(func(r rune) rune {
		if r >= 'a' && r <= 'z' || r >= '0' && r <= '9' || r == '-' {
			return r
		}
		return -1
	}, strings.ToLower(d))
	if len(d) > 16 {
		d = d[:16]
	}
	return d
}

func (a *Admin) authed(r *http.Request) bool {
	ok := func(t string) bool { return subtle.ConstantTimeCompare([]byte(t), []byte(a.token)) == 1 }
	if c, err := r.Cookie(adminCookie); err == nil && ok(c.Value) {
		return true
	}
	h := r.Header.Get("Authorization")
	return strings.HasPrefix(h, "Bearer ") && ok(strings.TrimPrefix(h, "Bearer "))
}

func (a *Admin) Handler() http.Handler {
	mux := http.NewServeMux()
	mux.HandleFunc("/", func(w http.ResponseWriter, r *http.Request) {
		if r.URL.Path != "/" {
			http.NotFound(w, r)
			return
		}
		if t := r.URL.Query().Get("token"); t != "" {
			if subtle.ConstantTimeCompare([]byte(t), []byte(a.token)) != 1 {
				http.Error(w, "wrong token", http.StatusForbidden)
				return
			}
			http.SetCookie(w, &http.Cookie{Name: adminCookie, Value: a.token, Path: "/", HttpOnly: true,
				SameSite: http.SameSiteStrictMode})
			if d := r.URL.Query().Get("device"); d != "" { // (the admin apps say which they are: the action records show it)
				http.SetCookie(w, &http.Cookie{Name: deviceCookie, Value: cleanDevice(d), Path: "/", HttpOnly: true,
					SameSite: http.SameSiteStrictMode})
			}
			http.Redirect(w, r, "/", http.StatusSeeOther)
			return
		}
		if !a.authed(r) {
			w.Header().Set("Content-Type", "text/html; charset=utf-8")
			w.WriteHeader(http.StatusUnauthorized)
			fmt.Fprint(w, "<!doctype html><meta charset=utf-8><body style='background:#0f1115;color:#e7e9ee;font-family:sans-serif;padding:40px'>관리 앱으로 열어 주세요.</body>")
			return
		}
		b, _ := adminFiles.ReadFile("admin/index.html")
		w.Header().Set("Content-Type", "text/html; charset=utf-8")
		w.Header().Set("Cache-Control", "no-store")
		w.Header().Set("Content-Security-Policy", "default-src 'self'; style-src 'self' 'unsafe-inline'; script-src 'self' 'unsafe-inline'")
		w.Write(b)
	})
	mux.HandleFunc("/api/state", func(w http.ResponseWriter, r *http.Request) {
		if !a.authed(r) {
			http.Error(w, "forbidden", http.StatusForbidden)
			return
		}
		w.Header().Set("Content-Type", "application/json")
		w.Header().Set("Cache-Control", "no-store")
		json.NewEncoder(w).Encode(a.Snapshot())
	})
	mux.HandleFunc("/api/bans", a.get(func(r *http.Request) any { return a.st.banList() }))
	mux.HandleFunc("/api/joins", a.get(func(r *http.Request) any { return a.st.findJoins(r.URL.Query().Get("q"), 500) }))
	mux.HandleFunc("/api/actions", a.get(func(r *http.Request) any { return a.st.actionList(300) }))
	mux.HandleFunc("/api/kick", a.post(a.kick))
	mux.HandleFunc("/api/close", a.post(a.closeRoom))
	mux.HandleFunc("/api/ban", a.post(a.ban))
	mux.HandleFunc("/api/unban", a.post(a.unban))
	mux.HandleFunc("/api/events", func(w http.ResponseWriter, r *http.Request) {
		if !a.authed(r) {
			http.Error(w, "forbidden", http.StatusForbidden)
			return
		}
		fl, ok := w.(http.Flusher)
		if !ok {
			http.Error(w, "no streaming", http.StatusInternalServerError)
			return
		}
		w.Header().Set("Content-Type", "text/event-stream")
		w.Header().Set("Cache-Control", "no-store")
		send := func() bool {
			b, _ := json.Marshal(a.Snapshot())
			_, err := fmt.Fprintf(w, "event: state\ndata: %s\n\n", b)
			fl.Flush()
			return err == nil
		}
		if !send() {
			return
		}
		beat := time.NewTicker(15 * time.Second)
		defer beat.Stop()
		for {
			ch := a.s.changes.wait()
			select {
			case <-r.Context().Done():
				return
			case <-beat.C:
				if _, err := fmt.Fprint(w, ": beat\n\n"); err != nil {
					return
				}
				fl.Flush()
			case <-ch:
				// (a burst of changes, e.g. a player joining, goes out as one snapshot)
				select {
				case <-time.After(150 * time.Millisecond):
				case <-r.Context().Done():
					return
				}
				if !send() {
					return
				}
			}
		}
	})
	return mux
}

// ---- the snapshot the page shows

type AdminState struct {
	Server AdminServer `json:"server"`
	Rooms  []AdminRoom `json:"rooms"`
	Bans   []Ban       `json:"bans"`
}

type AdminServer struct {
	Started time.Time `json:"started"`
	Now     time.Time `json:"now"`
	Version string    `json:"version"`
	Conns   int       `json:"conns"`
	Hubs    int       `json:"hubs"`
	Links   int       `json:"links"` // spliced players
}

type AdminRoom struct {
	ID      string        `json:"id"`
	Opened  time.Time     `json:"opened"`
	Keyed   bool          `json:"keyed"` // a matchmaking key (the server knows only its hash)
	Proto   int           `json:"proto"`
	Nodes   int           `json:"nodes"`
	Max     int           `json:"max"`
	Members []AdminMember `json:"members"`
}

type AdminMember struct {
	Node   int        `json:"node"` // -1: joining (no node yet)
	Name   string     `json:"name"`
	ID     string     `json:"id,omitempty"`  // port id
	Net    string     `json:"net,omitempty"` // the address, hashed
	Joined *time.Time `json:"joined,omitempty"`
	Link   string     `json:"link"` // host, udp, tcp (through this server), lan (not through it), joining
	Pair   string     `json:"pair,omitempty"`
}

func nodeName(info []byte, i int) string {
	b := info[niNodes+i*nodeSize+nodeNameOff:][:0x20]
	if n := strings.IndexByte(string(b), 0); n >= 0 {
		b = b[:n]
	}
	if !utf8.Valid(b) {
		return strings.ToValidUTF8(string(b), "?")
	}
	return string(b)
}

func (a *Admin) Snapshot() AdminState {
	s := a.s
	st := AdminState{Server: AdminServer{Started: a.started, Now: time.Now(), Version: a.version}, Rooms: []AdminRoom{},
		Bans: a.st.banList()}
	s.mu.Lock()
	defer s.mu.Unlock()
	st.Server.Conns, st.Server.Hubs, st.Server.Links = s.conns, len(s.hubs), len(s.pairs)
	byHub := map[*hub][]*pair{}
	for _, p := range s.pairs {
		byHub[p.hub] = append(byHub[p.hub], p)
	}
	for _, h := range s.hubs {
		r := AdminRoom{ID: fmt.Sprintf("%016x", h.id), Opened: h.created, Keyed: h.room != [16]byte{}, Proto: int(h.proto),
			Nodes: int(h.info[niNodeCount]), Max: int(h.info[niNodeMax]), Members: []AdminMember{}}
		opened := h.created
		r.Members = append(r.Members, AdminMember{Node: 0, Name: nodeName(h.info[:], 0), ID: h.pid, Net: h.net, Joined: &opened, Link: "host"})
		ps := byHub[h]
		for i := 1; i < r.Max && i < maxNodes; i++ {
			if !h.linked[i] {
				continue
			}
			m := AdminMember{Node: i, Name: nodeName(h.info[:], i), Link: "lan"}
			for _, p := range ps {
				if p.node == i {
					m.ID, m.Net, m.Pair, m.Link = p.pid, p.net, fmt.Sprintf("%08x", p.id), p.link()
					j := p.joined
					m.Joined = &j
				}
			}
			r.Members = append(r.Members, m)
		}
		for _, p := range ps {
			if p.node < 0 {
				j := p.joined
				r.Members = append(r.Members, AdminMember{Node: -1, ID: p.pid, Net: p.net, Joined: &j, Link: "joining",
					Pair: fmt.Sprintf("%08x", p.id)})
			}
		}
		st.Rooms = append(st.Rooms, r)
	}
	sort.Slice(st.Rooms, func(i, j int) bool { return st.Rooms[i].Opened.Before(st.Rooms[j].Opened) })
	return st
}

// link: UDP once both ends have sent a datagram, else the TCP link (both through this server).
func (p *pair) link() string {
	p.mu.Lock()
	defer p.mu.Unlock()
	if p.addr[0] != nil && p.addr[1] != nil {
		return "udp"
	}
	return "tcp"
}

// ---- actions

type actionReq struct {
	ID     string `json:"id"`  // port id
	Net    string `json:"net"` // hashed address (a ban: refuse it too; kick: close its connections too)
	Pair   string `json:"pair"`
	Room   string `json:"room"`
	Name   string `json:"name"`
	Reason string `json:"reason"`
	Days   int    `json:"days"` // 0: for good
}

type actionResp struct {
	OK      bool   `json:"ok"`
	Message string `json:"message,omitempty"`
	Closed  int    `json:"closed,omitempty"`
}

func (a *Admin) get(f func(*http.Request) any) http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		if !a.authed(r) {
			http.Error(w, "forbidden", http.StatusForbidden)
			return
		}
		w.Header().Set("Content-Type", "application/json")
		w.Header().Set("Cache-Control", "no-store")
		json.NewEncoder(w).Encode(f(r))
	}
}

// post: JSON in and out; the token, and (from a browser) the page's own origin.
func (a *Admin) post(f func(*http.Request, actionReq) actionResp) http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		if r.Method != http.MethodPost {
			http.Error(w, "POST", http.StatusMethodNotAllowed)
			return
		}
		if !a.authed(r) {
			http.Error(w, "forbidden", http.StatusForbidden)
			return
		}
		if o := r.Header.Get("Origin"); o != "" && o != "http://"+r.Host {
			http.Error(w, "origin", http.StatusForbidden)
			return
		}
		var req actionReq
		if err := json.NewDecoder(http.MaxBytesReader(w, r.Body, 4096)).Decode(&req); err != nil {
			http.Error(w, "bad request", http.StatusBadRequest)
			return
		}
		req.Reason = strings.TrimSpace(req.Reason)
		if len([]rune(req.Reason)) > 200 {
			req.Reason = string([]rune(req.Reason)[:200])
		}
		w.Header().Set("Content-Type", "application/json")
		json.NewEncoder(w).Encode(f(r, req))
	}
}

func device(r *http.Request) string {
	d := r.Header.Get("X-Admin-Device")
	if d == "" {
		if c, err := r.Cookie(deviceCookie); err == nil {
			d = c.Value
		}
	}
	if d = cleanDevice(d); d == "" {
		return "web"
	}
	return d
}

// validID: a port id ("ABCD-EFGH", portID) or a hashed address (12 hex digits, netOf), as the page sends them.
func validID(id string) bool {
	if len(id) != 9 || id[4] != '-' {
		return false
	}
	for i, c := range id {
		if i != 4 && !(c >= 'A' && c <= 'Z' || c >= '2' && c <= '7') {
			return false
		}
	}
	return true
}

func validNet(nh string) bool {
	if len(nh) != 12 {
		return false
	}
	for _, c := range nh {
		if !(c >= '0' && c <= '9' || c >= 'a' && c <= 'f') {
			return false
		}
	}
	return true
}

func target(id, nh string) string {
	if id != "" {
		return id
	}
	return "net:" + nh
}

func (a *Admin) kick(r *http.Request, q actionReq) actionResp {
	if q.Pair != "" {
		var id uint32
		if _, err := fmt.Sscanf(q.Pair, "%08x", &id); err != nil || !a.s.KickPair(id) {
			return actionResp{Message: "그 연결은 이미 끝났습니다"}
		}
		a.st.act(Action{At: time.Now(), Kind: "kick", Target: target(q.ID, q.Net), Name: q.Name, Reason: q.Reason, Device: device(r)})
		return actionResp{OK: true, Closed: 1}
	}
	if !validID(q.ID) {
		return actionResp{Message: "포트 ID가 없습니다"}
	}
	n := a.s.Kick(q.ID, "")
	a.st.act(Action{At: time.Now(), Kind: "kick", Target: q.ID, Name: q.Name, Reason: q.Reason, Device: device(r)})
	return actionResp{OK: true, Closed: n}
}

func (a *Admin) closeRoom(r *http.Request, q actionReq) actionResp {
	var id uint64
	if _, err := fmt.Sscanf(q.Room, "%016x", &id); err != nil || !a.s.CloseHub(id) {
		return actionResp{Message: "그 방은 이미 닫혔습니다"}
	}
	a.st.act(Action{At: time.Now(), Kind: "close", Target: q.Room, Name: q.Name, Reason: q.Reason, Device: device(r)})
	return actionResp{OK: true}
}

// ban: a port id, and (net given) the hashed address it came from too - a new port id from there is refused as well.
func (a *Admin) ban(r *http.Request, q actionReq) actionResp {
	if q.ID != "" && !validID(q.ID) || q.Net != "" && !validNet(q.Net) || q.ID == "" && q.Net == "" {
		return actionResp{Message: "포트 ID가 없는 사람(LAN·직접)은 밴할 수 없습니다"}
	}
	b := Ban{ID: q.ID, Net: q.Net, Name: q.Name, Reason: q.Reason, At: time.Now()}
	if q.Days > 0 {
		u := b.At.Add(time.Duration(q.Days) * 24 * time.Hour)
		b.Until = &u
	}
	a.st.ban(b)
	n := a.s.Kick(q.ID, q.Net)
	a.st.act(Action{At: b.At, Kind: "ban", Target: target(q.ID, q.Net), Name: q.Name,
		Reason: strings.TrimSpace(fmt.Sprintf("%s (%s)", q.Reason, banSpan(q.Days))), Device: device(r)})
	a.s.changes.notify()
	return actionResp{OK: true, Closed: n}
}

func banSpan(days int) string {
	if days <= 0 {
		return "영구"
	}
	return fmt.Sprintf("%d일", days)
}

func (a *Admin) unban(r *http.Request, q actionReq) actionResp {
	if !a.st.unban(q.ID, q.Net) {
		return actionResp{Message: "밴 목록에 없습니다"}
	}
	a.st.act(Action{At: time.Now(), Kind: "unban", Target: target(q.ID, q.Net), Name: q.Name, Reason: q.Reason, Device: device(r)})
	a.s.changes.notify()
	return actionResp{OK: true}
}
