// The lobby / relay server for MHGURecomp's Local Play over the internet (runtime/net/ldn.c "internet hubs").
//
// A hub's host keeps one connection here (HOST): its hub is listed for players with the same matchmaking key and
// port version (LIST). A player who picks it (JOIN) gets a ticket sent to the host, the host opens a second
// connection with that ticket (ATTACH), and from then on the two connections are spliced: the bytes are the same
// link the games speak over a direct connection (ldn.c link frames), which this server neither reads nor changes.
// Game packets go by UDP when that gets through: datagrams to this server's UDP port, prefixed with the pair's id
// and the sender's secret, are passed to the other end of the pair (without the prefix). Nobody needs a forwarded
// port, and players never see each other's IP addresses.
//
// Control frames: u32 size (of what follows), u8 type, payload; little endian. The first frame of a connection is a
// request, whose payload starts with a header: "MHGS", u16 server protocol, u16 link protocol (the port's
// PROTO_VERSION: only hubs of the same one are listed or joined), room[16] (the first 16 bytes of SHA-256 of the
// matchmaking key, all zero without a key), port id[16] (since server protocol 2: random bytes the port made when
// its player agreed to the hub server's use of their data - the port asks first and never connects without it).
//
// What is kept of a player (docs/hub-admin.md, "개인정보"): their port id, as a short code (portID), and a keyed hash
// of their IP address (netOf, the admin's secret key: the address itself is never written or shown, only whether two
// connections came from the same one). Addresses are in memory only while connected (limits, kicking).
//
// Everything a client sends is untrusted: sizes are capped, NetworkInfo is checked before it is listed, requests are
// rate limited per IP address, connections and hubs per IP address are limited, and the relay only ever forwards
// between the two ends of a pair it made itself (never to an address a client names; a relayed datagram is smaller
// than the one that caused it).
package main

import (
	"bytes"
	"crypto/rand"
	"crypto/sha256"
	"encoding/base32"
	"encoding/binary"
	"errors"
	"fmt"
	"io"
	"log"
	mrand "math/rand/v2"
	"net"
	"sort"
	"sync"
	"time"
)

const (
	serverProto = 2 // 2: the port id (consent); a port of protocol 1 (0.10.15 or before) is told errVersion: update
	headerSize  = 40
	netInfoSize = 0x480
	maxFrame    = 4096

	// requests (the first frame)
	tList   = 1 // header -> HUBS
	tHost   = 2 // header, NetworkInfo -> HOSTED; then UPDATE / PING from the host, INCOMING / PING to it
	tAttach = 3 // header, u64 ticket -> SPLICED (the host's side of a join)
	tJoin   = 4 // header, u64 hub id -> SPLICED (the player's side) or ERROR
	// on a hub's connection
	tUpdate = 5 // NetworkInfo: the hub changed (players, advertise data)
	tPing   = 6 // keeps the connection alive (both ways)
	// answers
	tHubs     = 16 // u32 count, count x (u64 hub id, NetworkInfo)
	tHosted   = 17 // u64 hub id
	tIncoming = 18 // u64 ticket: a player joins, ATTACH with it
	tSpliced  = 19 // u32 pair id, u64 secret (for UDP); then the connection is the link
	tError    = 20 // u32 code

	errNotFound = 1 // no such hub (or not for this key / version)
	errNoAnswer = 2 // the host did not take the player
	errBusy     = 3 // a limit was reached
	errVersion  = 4 // server protocol mismatch
	errBadReq   = 5
	errBanned   = 6 // (the port shows it as a refusal)

	udpMagic  = "MHGR"
	udpHeader = 16 // magic, u32 pair, u64 secret
	udpMax    = udpHeader + 12 + 16 + 8192

	// NetworkInfo offsets (runtime/net/ldn.c)
	niSSID      = 0x26
	niNetType   = 0x4B
	niNodeMax   = 0x66
	niNodeCount = 0x67
	niNodes     = 0x68 // maxNodes x NodeInfo
	niAdvSize   = 0x26A
	nodeSize    = 0x40
	nodeLinked  = 0x0B // NodeInfo: u8 connected
	nodeNameOff = 0x0C // NodeInfo: user name[0x20] (the port's nickname)
	advMax      = 0x180
	maxNodes    = 8
)

type Config struct {
	Addr          string
	MaxConns      int     // all connections
	MaxConnsPerIP int     // connections from one IP address
	MaxHubs       int     // hubs listed at once
	MaxHubsPerIP  int     // hubs one IP address hosts
	ListMax       int     // hubs in one LIST answer (the game's Join Hub list shows 18: 3 pages of 6)
	ReqRate       float64 // LIST / JOIN requests a second per IP address (burst: ReqBurst)
	ReqBurst      float64
	Bandwidth     float64       // bytes a second per pair and direction (TCP), and per pair (UDP)
	HandshakeTime time.Duration // for the first frame, and a joining player's wait for the host
	PingEvery     time.Duration
	Verbose       bool
}

func DefaultConfig() Config {
	return Config{
		Addr: ":39510", MaxConns: 4000, MaxConnsPerIP: 24, MaxHubs: 2000, MaxHubsPerIP: 4, ListMax: 18,
		ReqRate: 2, ReqBurst: 15, Bandwidth: 1 << 20, HandshakeTime: 10 * time.Second, PingEvery: 15 * time.Second,
	}
}

type hub struct {
	id      uint64
	room    [16]byte
	proto   uint16
	info    [netInfoSize]byte // guarded by Server.mu
	created time.Time
	ip      string      // (memory only: the per-address hub limit)
	pid     string      // the host's port id (portID)
	net     string      // the host's address, hashed (netOf)
	out     chan []byte // frames to the host
	done    chan struct{}
	joins   int // pending and spliced players (guarded by Server.mu)
	conn    *net.TCPConn
	linked  [maxNodes]bool // the nodes connected in the last NetworkInfo (guarded by Server.mu; admin view)
}

type pending struct {
	hub      *hub
	host     chan *net.TCPConn // the host's ATTACH connection (buffered: never blocks)
	finished chan struct{}     // the splice ended (the ATTACH handler waits for it)
}

type pair struct {
	id     uint32
	secret [2]uint64 // 0: host, 1: player
	mu     sync.Mutex
	addr   [2]*net.UDPAddr
	lim    *bucket
	// for the admin view (guarded by Server.mu): the hub, the player's address, when, and which node of the hub's
	// NetworkInfo the player became (-1: not yet / unknown)
	hub    *hub
	pid    string // the player's port id
	net    string // the player's address, hashed
	joined time.Time
	node   int
	conns  [2]*net.TCPConn // host side, player side (closing either ends the link)
}

type ipState struct {
	conns, hubs int
	req         *bucket
	seen        time.Time
}

type Server struct {
	cfg     Config
	mu      sync.Mutex
	hubs    map[uint64]*hub
	pending map[uint64]*pending
	pairs   map[uint32]*pair
	ips     map[string]*ipState
	conns   int
	ln      *net.TCPListener
	udp     *net.UDPConn
	changes changes // hubs, players or links changed (the admin view's live updates)

	// the admin view: every open connection (guarded by mu), and hooks it sets before Serve (set and read with mu held)
	live     map[*net.TCPConn]*liveConn
	netOf    func(ip string) string     // an address's keyed hash (nil: no admin view, "")
	banned   func(net, pid string) bool // a connection from this address or port id is closed at once
	onRecord func(JoinRecord)           // a host opened a hub, or a player became a node (called with mu held: don't block)
}

type liveConn struct {
	ip, net, pid string // (pid "" until the request is read)
}

// portID: the code a port's id is shown and banned by ("ABCD-EFGH": 40 bits of its SHA-256; the id itself is kept
// nowhere).
func portID(id []byte) string {
	sum := sha256.Sum256(id)
	c := base32.StdEncoding.EncodeToString(sum[:5])
	return c[:4] + "-" + c[4:8]
}

// JoinRecord: who came in, for the admin view's records (kept 3 days, docs/hub-admin.md).
type JoinRecord struct {
	At   time.Time `json:"at"`
	Name string    `json:"name"`
	ID   string    `json:"id"`           // port id
	Net  string    `json:"net"`          // the address, hashed
	IP   string    `json:"ip,omitempty"` // (only in records from before: openStore hashes it)
	Room string    `json:"room"`
	Host string    `json:"host"` // the room's host's name
	Role string    `json:"role"` // host, player
}

func NewServer(cfg Config) *Server {
	return &Server{cfg: cfg, hubs: map[uint64]*hub{}, pending: map[uint64]*pending{}, pairs: map[uint32]*pair{},
		ips: map[string]*ipState{}, live: map[*net.TCPConn]*liveConn{}}
}

func (s *Server) vlog(format string, args ...any) {
	if s.cfg.Verbose {
		log.Printf(format, args...)
	}
}

// Listen opens the TCP and UDP ports (the same number); Serve then runs until Close.
func (s *Server) Listen() error {
	ta, err := net.ResolveTCPAddr("tcp", s.cfg.Addr)
	if err != nil {
		return err
	}
	if s.ln, err = net.ListenTCP("tcp", ta); err != nil {
		return err
	}
	ua := &net.UDPAddr{IP: ta.IP, Port: s.ln.Addr().(*net.TCPAddr).Port}
	if s.udp, err = net.ListenUDP("udp", ua); err != nil {
		s.ln.Close()
		return err
	}
	return nil
}

func (s *Server) Addr() *net.TCPAddr { return s.ln.Addr().(*net.TCPAddr) }

func (s *Server) Close() {
	s.ln.Close()
	s.udp.Close()
}

func (s *Server) Serve() error {
	go s.serveUDP()
	go s.janitor()
	for {
		c, err := s.ln.AcceptTCP()
		if err != nil {
			if errors.Is(err, net.ErrClosed) {
				return nil
			}
			time.Sleep(50 * time.Millisecond)
			continue
		}
		go s.handle(c)
	}
}

// Forgets the request budgets of addresses not seen for a while.
func (s *Server) janitor() {
	for range time.Tick(time.Minute) {
		s.mu.Lock()
		for ip, st := range s.ips {
			if st.conns == 0 && st.hubs == 0 && time.Since(st.seen) > 5*time.Minute {
				delete(s.ips, ip)
			}
		}
		s.mu.Unlock()
	}
}

func rand64() uint64 {
	var b [8]byte
	if _, err := rand.Read(b[:]); err != nil {
		panic(err)
	}
	return binary.LittleEndian.Uint64(b[:])
}

// ---- frames
func readFrame(r io.Reader, max int) (byte, []byte, error) {
	var h [5]byte
	if _, err := io.ReadFull(r, h[:4]); err != nil {
		return 0, nil, err
	}
	size := binary.LittleEndian.Uint32(h[:4])
	if size < 1 || size > uint32(max)+1 {
		return 0, nil, errors.New("bad frame size")
	}
	p := make([]byte, size)
	if _, err := io.ReadFull(r, p); err != nil {
		return 0, nil, err
	}
	return p[0], p[1:], nil
}

func frame(t byte, payload ...[]byte) []byte {
	n := 1
	for _, p := range payload {
		n += len(p)
	}
	b := make([]byte, 5, 4+n)
	binary.LittleEndian.PutUint32(b, uint32(n))
	b[4] = t
	for _, p := range payload {
		b = append(b, p...)
	}
	return b
}

func writeFrame(c net.Conn, f []byte) error {
	c.SetWriteDeadline(time.Now().Add(10 * time.Second))
	_, err := c.Write(f)
	return err
}

func u32(v uint32) []byte { return binary.LittleEndian.AppendUint32(nil, v) }
func u64(v uint64) []byte { return binary.LittleEndian.AppendUint64(nil, v) }

func sendError(c net.Conn, code uint32) { writeFrame(c, frame(tError, u32(code))) }

// infoOK: a NetworkInfo fit to be shown to other players (the game parses it).
func infoOK(i []byte) bool {
	if len(i) != netInfoSize {
		return false
	}
	max, count := i[niNodeMax], i[niNodeCount]
	return i[niNetType] == 2 && i[niSSID] <= 32 && max >= 1 && max <= maxNodes && count >= 1 && count <= max &&
		binary.LittleEndian.Uint16(i[niAdvSize:]) <= advMax
}

// ---- connections
func (s *Server) admit(ip string) bool {
	s.mu.Lock()
	defer s.mu.Unlock()
	st := s.ips[ip]
	if st == nil {
		st = &ipState{req: newBucket(s.cfg.ReqRate, s.cfg.ReqBurst)}
		s.ips[ip] = st
	}
	st.seen = time.Now()
	if s.conns >= s.cfg.MaxConns || st.conns >= s.cfg.MaxConnsPerIP {
		return false
	}
	s.conns++
	st.conns++
	return true
}

func (s *Server) release(ip string) {
	s.mu.Lock()
	s.conns--
	if st := s.ips[ip]; st != nil {
		st.conns--
		st.seen = time.Now()
	}
	s.mu.Unlock()
}

func (s *Server) allowRequest(ip string) bool {
	s.mu.Lock()
	st := s.ips[ip]
	s.mu.Unlock()
	return st != nil && st.req.take(1)
}

func (s *Server) handle(c *net.TCPConn) {
	ip := c.RemoteAddr().(*net.TCPAddr).IP.String()
	s.mu.Lock()
	banned, netOf := s.banned, s.netOf
	s.mu.Unlock()
	nh := ""
	if netOf != nil {
		nh = netOf(ip)
	}
	if banned != nil && banned(nh, "") {
		c.Close()
		return
	}
	if !s.admit(ip) {
		c.Close()
		return
	}
	defer s.release(ip)
	defer c.Close()
	lc := &liveConn{ip: ip, net: nh}
	s.mu.Lock()
	s.live[c] = lc
	s.mu.Unlock()
	defer func() {
		s.mu.Lock()
		delete(s.live, c)
		s.mu.Unlock()
	}()
	c.SetNoDelay(true)
	c.SetKeepAlive(true)
	c.SetKeepAlivePeriod(30 * time.Second)
	c.SetReadDeadline(time.Now().Add(s.cfg.HandshakeTime))
	t, p, err := readFrame(c, maxFrame)
	if err != nil || len(p) < 8 || !bytes.Equal(p[:4], []byte("MHGS")) {
		return
	}
	if binary.LittleEndian.Uint16(p[4:]) != serverProto {
		sendError(c, errVersion)
		return
	}
	if len(p) < headerSize {
		sendError(c, errBadReq)
		return
	}
	if bytes.Equal(p[24:40], make([]byte, 16)) { // (no consent: a port never sends this)
		sendError(c, errBadReq)
		return
	}
	pid := portID(p[24:40])
	if banned != nil && banned("", pid) {
		sendError(c, errBanned)
		return
	}
	s.mu.Lock()
	lc.pid = pid
	s.mu.Unlock()
	proto := binary.LittleEndian.Uint16(p[6:])
	var room [16]byte
	copy(room[:], p[8:24])
	body := p[headerSize:]
	switch t {
	case tList:
		if !s.allowRequest(ip) {
			sendError(c, errBusy)
			return
		}
		s.list(c, proto, room)
	case tHost:
		if len(body) != netInfoSize || !infoOK(body) {
			sendError(c, errBadReq)
			return
		}
		s.host(c, ip, lc, proto, room, body)
	case tAttach:
		if len(body) != 8 {
			return
		}
		s.attach(c, binary.LittleEndian.Uint64(body))
	case tJoin:
		if len(body) != 8 {
			return
		}
		if !s.allowRequest(ip) {
			sendError(c, errBusy)
			return
		}
		s.join(c, lc, proto, room, binary.LittleEndian.Uint64(body))
	}
}

// The hubs a player with this key and version sees: the ones with room for more players first, then the fullest,
// then the newest.
func (s *Server) list(c *net.TCPConn, proto uint16, room [16]byte) {
	type entry struct {
		id      uint64
		info    [netInfoSize]byte
		created time.Time
	}
	var es []entry
	s.mu.Lock()
	for _, h := range s.hubs {
		if h.proto == proto && h.room == room {
			es = append(es, entry{h.id, h.info, h.created})
		}
	}
	s.mu.Unlock()
	free := func(e *entry) bool { return e.info[niNodeCount] < e.info[niNodeMax] }
	// more hubs than one answer holds: a random ListMax of them, those with a free place before full ones, so each
	// search (the game's Retry) can show others
	if len(es) > s.cfg.ListMax {
		mrand.Shuffle(len(es), func(i, j int) { es[i], es[j] = es[j], es[i] })
		sort.SliceStable(es, func(i, j int) bool { return free(&es[i]) && !free(&es[j]) })
		es = es[:s.cfg.ListMax]
	}
	sort.Slice(es, func(i, j int) bool {
		a, b := &es[i], &es[j]
		if free(a) != free(b) {
			return free(a)
		}
		if a.info[niNodeCount] != b.info[niNodeCount] {
			return a.info[niNodeCount] > b.info[niNodeCount]
		}
		return a.created.After(b.created)
	})
	out := u32(uint32(len(es)))
	for i := range es {
		out = append(out, u64(es[i].id)...)
		out = append(out, es[i].info[:]...)
	}
	writeFrame(c, frame(tHubs, out))
}

func (s *Server) host(c *net.TCPConn, ip string, lc *liveConn, proto uint16, room [16]byte, info []byte) {
	h := &hub{room: room, proto: proto, created: time.Now(), ip: ip, pid: lc.pid, net: lc.net, out: make(chan []byte, 16),
		done: make(chan struct{})}
	copy(h.info[:], info)
	s.mu.Lock()
	st := s.ips[ip]
	if len(s.hubs) >= s.cfg.MaxHubs || st.hubs >= s.cfg.MaxHubsPerIP {
		s.mu.Unlock()
		sendError(c, errBusy)
		return
	}
	for h.id == 0 || s.hubs[h.id] != nil {
		h.id = rand64()
	}
	h.conn = c
	s.hubs[h.id] = h
	st.hubs++
	s.linkNodes(h)
	if s.onRecord != nil {
		s.onRecord(JoinRecord{At: h.created, Name: nodeName(h.info[:], 0), ID: h.pid, Net: h.net, Room: fmt.Sprintf("%016x", h.id),
			Host: nodeName(h.info[:], 0), Role: "host"})
	}
	s.mu.Unlock()
	s.changes.notify()
	s.vlog("hub %016x up (%d players)", h.id, h.info[niNodeCount])
	defer func() {
		s.mu.Lock()
		delete(s.hubs, h.id)
		if st := s.ips[ip]; st != nil {
			st.hubs--
		}
		s.mu.Unlock()
		close(h.done)
		s.changes.notify()
		s.vlog("hub %016x down", h.id)
	}()
	if writeFrame(c, frame(tHosted, u64(h.id))) != nil {
		return
	}
	// writer: tickets and pings; it closes the connection on a write error, which ends the reader too
	go func() {
		t := time.NewTicker(s.cfg.PingEvery)
		defer t.Stop()
		for {
			var f []byte
			select {
			case <-h.done:
				return
			case f = <-h.out:
			case <-t.C:
				f = frame(tPing)
			}
			if writeFrame(c, f) != nil {
				c.Close()
				return
			}
		}
	}()
	for {
		c.SetReadDeadline(time.Now().Add(3 * s.cfg.PingEvery))
		t, p, err := readFrame(c, maxFrame)
		if err != nil {
			return
		}
		switch t {
		case tUpdate:
			if !infoOK(p) {
				return
			}
			s.mu.Lock()
			copy(h.info[:], p)
			s.linkNodes(h)
			s.mu.Unlock()
			s.changes.notify()
		case tPing:
		default:
			return
		}
	}
}

func (s *Server) join(c *net.TCPConn, lc *liveConn, proto uint16, room [16]byte, id uint64) {
	s.mu.Lock()
	h := s.hubs[id]
	if h == nil || h.room != room || h.proto != proto {
		s.mu.Unlock()
		sendError(c, errNotFound)
		return
	}
	if h.joins >= maxNodes-1 {
		s.mu.Unlock()
		sendError(c, errBusy)
		return
	}
	var ticket uint64
	for ticket == 0 || s.pending[ticket] != nil {
		ticket = rand64()
	}
	pj := &pending{hub: h, host: make(chan *net.TCPConn, 1), finished: make(chan struct{})}
	s.pending[ticket] = pj
	h.joins++
	s.mu.Unlock()
	defer func() {
		s.mu.Lock()
		h.joins--
		delete(s.pending, ticket)
		s.mu.Unlock()
		close(pj.finished)
	}()
	select {
	case h.out <- frame(tIncoming, u64(ticket)):
	default:
		sendError(c, errBusy)
		return
	}
	var hc *net.TCPConn
	timeout := time.NewTimer(s.cfg.HandshakeTime)
	defer timeout.Stop()
	select {
	case hc = <-pj.host:
	case <-h.done:
	case <-timeout.C:
	}
	if hc == nil {
		s.mu.Lock()
		_, waiting := s.pending[ticket]
		delete(s.pending, ticket)
		s.mu.Unlock()
		if waiting { // (else the host's ATTACH took the ticket just now: its connection is on the way)
			sendError(c, errNoAnswer)
			return
		}
		hc = <-pj.host
	}
	p := &pair{secret: [2]uint64{rand64(), rand64()}, lim: newBucket(s.cfg.Bandwidth, s.cfg.Bandwidth), hub: h,
		pid: lc.pid, net: lc.net, joined: time.Now(), node: -1, conns: [2]*net.TCPConn{hc, c}}
	s.mu.Lock()
	for p.id == 0 || s.pairs[p.id] != nil {
		p.id = uint32(rand64())
	}
	s.pairs[p.id] = p
	s.mu.Unlock()
	s.changes.notify()
	defer func() {
		s.mu.Lock()
		delete(s.pairs, p.id)
		s.mu.Unlock()
		s.changes.notify()
	}()
	if writeFrame(hc, frame(tSpliced, u32(p.id), u64(p.secret[0]))) != nil ||
		writeFrame(c, frame(tSpliced, u32(p.id), u64(p.secret[1]))) != nil {
		hc.Close()
		return
	}
	s.vlog("hub %016x: player joined (pair %08x)", id, p.id)
	s.splice(hc, c)
	s.vlog("hub %016x: player left (pair %08x)", id, p.id)
}

// The host's connection for a ticket: handed to the waiting join, which splices it; it stays open until then.
func (s *Server) attach(c *net.TCPConn, ticket uint64) {
	s.mu.Lock()
	pj := s.pending[ticket]
	delete(s.pending, ticket)
	s.mu.Unlock()
	if pj == nil {
		sendError(c, errNotFound)
		return
	}
	pj.host <- c
	<-pj.finished
}

// Copies both ways until either end closes, at most Bandwidth bytes a second each way.
func (s *Server) splice(a, b *net.TCPConn) {
	a.SetDeadline(time.Time{})
	b.SetDeadline(time.Time{})
	done := make(chan struct{}, 2)
	pipe := func(dst, src *net.TCPConn) {
		lim := newBucket(s.cfg.Bandwidth, s.cfg.Bandwidth)
		buf := make([]byte, 16<<10)
		for {
			n, err := src.Read(buf)
			if n > 0 {
				if d := lim.wait(float64(n)); d > 0 {
					time.Sleep(d)
				}
				dst.SetWriteDeadline(time.Now().Add(60 * time.Second))
				if _, werr := dst.Write(buf[:n]); werr != nil {
					break
				}
			}
			if err != nil {
				break
			}
		}
		done <- struct{}{}
	}
	go pipe(a, b)
	go pipe(b, a)
	<-done
	a.Close()
	b.Close()
	<-done
}

// ---- UDP relay
func (s *Server) serveUDP() {
	buf := make([]byte, udpMax+1)
	for {
		n, from, err := s.udp.ReadFromUDP(buf)
		if err != nil {
			if errors.Is(err, net.ErrClosed) {
				return
			}
			continue
		}
		if n < udpHeader || n > udpMax || string(buf[:4]) != udpMagic {
			continue
		}
		id := binary.LittleEndian.Uint32(buf[4:])
		secret := binary.LittleEndian.Uint64(buf[8:])
		s.mu.Lock()
		p := s.pairs[id]
		s.mu.Unlock()
		if p == nil {
			continue
		}
		side := -1
		for i, sec := range p.secret {
			if sec == secret {
				side = i
			}
		}
		if side < 0 {
			continue
		}
		p.mu.Lock()
		first := p.addr[side] == nil
		p.addr[side] = from // (NAT may change it: the latest wins)
		to := p.addr[1-side]
		p.mu.Unlock()
		if first {
			s.changes.notify() // (the admin view shows UDP once both ends are known)
		}
		// (an empty datagram only tells where its sender is)
		if n > udpHeader && to != nil && p.lim.take(float64(n)) {
			s.udp.WriteToUDP(buf[udpHeader:n], to)
		}
	}
}

// ---- the admin view's bookkeeping

// linkNodes: which player is which node. A node that appears in the hub's NetworkInfo after a player was spliced is
// that player (the newest unplaced one first); a node that goes away frees its player. Players who came over the LAN
// or by address have no pair: their nodes stay without an address. Called with s.mu held.
func (s *Server) linkNodes(h *hub) {
	var now [maxNodes]bool
	n := int(h.info[niNodeMax])
	for i := 0; i < n && i < maxNodes; i++ {
		now[i] = i == 0 || h.info[niNodes+i*nodeSize+nodeLinked] != 0
	}
	for _, p := range s.pairs {
		if p.hub == h && p.node > 0 && !now[p.node] {
			p.node = -1
		}
	}
	for i := 1; i < maxNodes; i++ {
		if !now[i] || h.linked[i] {
			continue
		}
		var best *pair
		for _, p := range s.pairs {
			if p.hub == h && p.node < 0 && (best == nil || p.joined.After(best.joined)) {
				best = p
			}
		}
		if best != nil {
			best.node = i
			if s.onRecord != nil {
				s.onRecord(JoinRecord{At: best.joined, Name: nodeName(h.info[:], i), ID: best.pid, Net: best.net,
					Room: fmt.Sprintf("%016x", h.id), Host: nodeName(h.info[:], 0), Role: "player"})
			}
		}
	}
	h.linked = now
}

// changes: a channel closed (and replaced) on each change, for the admin view's live updates.
type changes struct {
	mu sync.Mutex
	ch chan struct{}
}

func (c *changes) wait() <-chan struct{} {
	c.mu.Lock()
	defer c.mu.Unlock()
	if c.ch == nil {
		c.ch = make(chan struct{})
	}
	return c.ch
}

func (c *changes) notify() {
	c.mu.Lock()
	if c.ch != nil {
		close(c.ch)
		c.ch = nil
	}
	c.mu.Unlock()
}

// ---- the admin view's actions (they close connections; the games see them drop)

// Kick closes every connection of this port id (or, net set, also from this hashed address); it can come back
// unless banned.
func (s *Server) Kick(pid, nh string) int {
	s.mu.Lock()
	var cs []*net.TCPConn
	for c, l := range s.live {
		if pid != "" && l.pid == pid || nh != "" && l.net == nh {
			cs = append(cs, c)
		}
	}
	s.mu.Unlock()
	for _, c := range cs {
		c.Close()
	}
	if len(cs) > 0 {
		s.changes.notify()
	}
	return len(cs)
}

// KickPair ends one player's link (the host's room stays).
func (s *Server) KickPair(id uint32) bool {
	s.mu.Lock()
	p := s.pairs[id]
	s.mu.Unlock()
	if p == nil {
		return false
	}
	p.conns[0].Close()
	p.conns[1].Close()
	return true
}

// CloseHub takes a room down: its host's hub connection and every player's link.
func (s *Server) CloseHub(id uint64) bool {
	s.mu.Lock()
	h := s.hubs[id]
	var cs []*net.TCPConn
	if h != nil {
		cs = append(cs, h.conn)
		for _, p := range s.pairs {
			if p.hub == h {
				cs = append(cs, p.conns[0], p.conns[1])
			}
		}
	}
	s.mu.Unlock()
	for _, c := range cs {
		c.Close()
	}
	return h != nil
}
