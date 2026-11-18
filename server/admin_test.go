package main

import (
	"bufio"
	"encoding/json"
	"net"
	"net/http"
	"net/http/cookiejar"
	"net/http/httptest"
	"strings"
	"testing"
	"time"
)

func namedInfo(names ...string) []byte {
	i := testInfo(byte(len(names)))
	for n, name := range names {
		nd := i[niNodes+n*nodeSize:]
		nd[nodeLinked] = 1
		copy(nd[nodeNameOff:nodeNameOff+0x20], name)
	}
	return i
}

func room(t *testing.T, st AdminState, id string) AdminRoom {
	t.Helper()
	for _, r := range st.Rooms {
		if r.ID == id {
			return r
		}
	}
	t.Fatalf("room %s not in %+v", id, st.Rooms)
	return AdminRoom{}
}

func TestAdminView(t *testing.T) {
	s := start(t, nil)
	a, err := NewAdmin(s, t.TempDir(), "test")
	if err != nil {
		t.Fatal(err)
	}
	hs := httptest.NewServer(a.Handler())
	defer hs.Close()

	// a hub with its host only
	hc := dial(t, s)
	send(t, hc, tHost, header(2, 0), namedInfo("방장"))
	idn := u64le(expect(t, hc, tHosted))
	id := hexid(idn)
	r := room(t, a.Snapshot(), id)
	if r.Keyed || r.Nodes != 1 || len(r.Members) != 1 || r.Members[0].Name != "방장" || r.Members[0].IP != "127.0.0.1" ||
		r.Members[0].Link != "host" {
		t.Fatalf("host only: %+v", r)
	}

	// a player joins through the server: first "joining", then the node that appears is that player
	pc := dial(t, s)
	send(t, pc, tJoin, header(2, 0), u64(idn))
	ticket := expect(t, hc, tIncoming)
	ac := dial(t, s)
	send(t, ac, tAttach, header(2, 0), ticket)
	hsec := expect(t, ac, tSpliced)
	psec := expect(t, pc, tSpliced)
	time.Sleep(50 * time.Millisecond)
	r = room(t, a.Snapshot(), id)
	if len(r.Members) != 2 || r.Members[1].Link != "joining" || r.Members[1].IP != "127.0.0.1" {
		t.Fatalf("joining: %+v", r.Members)
	}
	send(t, hc, tUpdate, namedInfo("방장", "손님"))
	time.Sleep(50 * time.Millisecond)
	r = room(t, a.Snapshot(), id)
	if r.Nodes != 2 || len(r.Members) != 2 || r.Members[1].Name != "손님" || r.Members[1].Link != "tcp" ||
		r.Members[1].IP != "127.0.0.1" || r.Members[1].Joined == nil {
		t.Fatalf("joined: %+v", r.Members)
	}

	// both ends announce UDP: the link shows udp
	ua := &net.UDPAddr{IP: net.IPv4(127, 0, 0, 1), Port: s.Addr().Port}
	hu, _ := net.DialUDP("udp", nil, ua)
	pu, _ := net.DialUDP("udp", nil, ua)
	defer hu.Close()
	defer pu.Close()
	hu.Write(append(append([]byte(udpMagic), hsec[:4]...), hsec[4:12]...))
	pu.Write(append(append([]byte(udpMagic), psec[:4]...), psec[4:12]...))
	time.Sleep(50 * time.Millisecond)
	if m := room(t, a.Snapshot(), id).Members[1]; m.Link != "udp" {
		t.Fatalf("udp: %+v", m)
	}

	// a player who came over the LAN: a node with no address
	send(t, hc, tUpdate, namedInfo("방장", "손님", "옆자리"))
	time.Sleep(50 * time.Millisecond)
	if m := room(t, a.Snapshot(), id).Members[2]; m.Name != "옆자리" || m.Link != "lan" || m.IP != "" {
		t.Fatalf("lan: %+v", m)
	}

	// the player leaves: the node goes, the hub stays
	pc.Close()
	send(t, hc, tUpdate, namedInfo("방장"))
	time.Sleep(100 * time.Millisecond)
	if r := room(t, a.Snapshot(), id); len(r.Members) != 1 {
		t.Fatalf("after leaving: %+v", r.Members)
	}

	// the page and the API need the token
	for _, path := range []string{"/", "/api/state", "/api/events"} {
		resp, err := http.Get(hs.URL + path)
		if err != nil {
			t.Fatal(err)
		}
		resp.Body.Close()
		if resp.StatusCode == http.StatusOK {
			t.Fatalf("%s without the token: %d", path, resp.StatusCode)
		}
	}
	req, _ := http.NewRequest("GET", hs.URL+"/api/state", nil)
	req.Header.Set("Authorization", "Bearer "+a.token)
	resp, err := http.DefaultClient.Do(req)
	if err != nil {
		t.Fatal(err)
	}
	var st AdminState
	json.NewDecoder(resp.Body).Decode(&st)
	resp.Body.Close()
	if st.Server.Hubs != 1 {
		t.Fatalf("api state %+v", st.Server)
	}
	// the token in the address sets the cookie and leads to the page
	jar, _ := cookiejar.New(nil)
	cl := &http.Client{Jar: jar}
	resp, err = cl.Get(hs.URL + "/?token=" + a.token)
	if err != nil {
		t.Fatal(err)
	}
	resp.Body.Close()
	if resp.StatusCode != http.StatusOK {
		t.Fatalf("page with the token: %d", resp.StatusCode)
	}

	// live updates: the first snapshot at once, another when a hub closes
	req, _ = http.NewRequest("GET", hs.URL+"/api/events", nil)
	req.Header.Set("Authorization", "Bearer "+a.token)
	resp, err = http.DefaultClient.Do(req)
	if err != nil {
		t.Fatal(err)
	}
	defer resp.Body.Close()
	sc := bufio.NewScanner(resp.Body)
	sc.Buffer(make([]byte, 1<<20), 1<<20)
	next := func() AdminState {
		for sc.Scan() {
			if l := sc.Text(); strings.HasPrefix(l, "data: ") {
				var st AdminState
				json.Unmarshal([]byte(l[6:]), &st)
				return st
			}
		}
		t.Fatal("event stream ended")
		return AdminState{}
	}
	if st := next(); len(st.Rooms) != 1 {
		t.Fatalf("first event %+v", st.Rooms)
	}
	hc.Close()
	if st := next(); len(st.Rooms) != 0 {
		t.Fatalf("after the hub closed %+v", st.Rooms)
	}
}

func u64le(b []byte) uint64 {
	var v uint64
	for i := 7; i >= 0; i-- {
		v = v<<8 | uint64(b[i])
	}
	return v
}

func hexid(v uint64) string { return strings.ToLower(strings.TrimSpace(fmtHex(v))) }

func fmtHex(v uint64) string {
	const d = "0123456789abcdef"
	b := make([]byte, 16)
	for i := 15; i >= 0; i-- {
		b[i] = d[v&15]
		v >>= 4
	}
	return string(b)
}

