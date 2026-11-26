package main

import (
	"bufio"
	"encoding/binary"
	"encoding/json"
	"net"
	"net/http"
	"net/http/cookiejar"
	"net/http/httptest"
	"os"
	"path/filepath"
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

	// a hub with its host only (shown by port id and a hash of the address: never the address)
	me := portID(headerAs(2, 0, 1)[24:40])
	hc := dial(t, s)
	send(t, hc, tHost, header(2, 0), namedInfo("방장"))
	idn := u64le(expect(t, hc, tHosted))
	id := hexid(idn)
	r := room(t, a.Snapshot(), id)
	if r.Keyed || r.Nodes != 1 || len(r.Members) != 1 || r.Members[0].Name != "방장" || r.Members[0].ID != me || !validNet(r.Members[0].Net) ||
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
	if len(r.Members) != 2 || r.Members[1].Link != "joining" || r.Members[1].ID != me {
		t.Fatalf("joining: %+v", r.Members)
	}
	send(t, hc, tUpdate, namedInfo("방장", "손님"))
	time.Sleep(50 * time.Millisecond)
	r = room(t, a.Snapshot(), id)
	if r.Nodes != 2 || len(r.Members) != 2 || r.Members[1].Name != "손님" || r.Members[1].Link != "tcp" ||
		r.Members[1].ID != me || r.Members[1].Net != r.Members[0].Net || r.Members[1].Joined == nil {
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
	if m := room(t, a.Snapshot(), id).Members[2]; m.Name != "옆자리" || m.Link != "lan" || m.ID != "" || m.Net != "" {
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

func post(t *testing.T, a *Admin, url, path string, body string) actionResp {
	t.Helper()
	req, _ := http.NewRequest("POST", url+path, strings.NewReader(body))
	req.Header.Set("Authorization", "Bearer "+a.token)
	req.Header.Set("Content-Type", "application/json")
	resp, err := http.DefaultClient.Do(req)
	if err != nil {
		t.Fatal(err)
	}
	defer resp.Body.Close()
	var r actionResp
	json.NewDecoder(resp.Body).Decode(&r)
	return r
}

func TestAdminActions(t *testing.T) {
	s := start(t, nil)
	dir := t.TempDir()
	a, err := NewAdmin(s, dir, "test")
	if err != nil {
		t.Fatal(err)
	}
	hs := httptest.NewServer(a.Handler())
	defer hs.Close()

	hc := dial(t, s)
	send(t, hc, tHost, header(2, 0), namedInfo("방장"))
	idn := u64le(expect(t, hc, tHosted))
	pc := dial(t, s)
	send(t, pc, tJoin, headerAs(2, 0, 2), u64(idn))
	ticket := expect(t, hc, tIncoming)
	ac := dial(t, s)
	send(t, ac, tAttach, header(2, 0), ticket)
	expect(t, ac, tSpliced)
	expect(t, pc, tSpliced)
	send(t, hc, tUpdate, namedInfo("방장", "치터"))
	time.Sleep(50 * time.Millisecond)

	// the records: the host and the player, by name; by port id too
	cheater := portID(headerAs(2, 0, 2)[24:40])
	if j := a.st.findJoins(strings.ToLower(cheater), 10); len(j) != 1 || j[0].Name != "치터" {
		t.Fatalf("records by id %+v", j)
	}
	if j := a.st.findJoins("치터", 10); len(j) != 1 || j[0].Role != "player" || j[0].Host != "방장" || j[0].ID != cheater || !validNet(j[0].Net) || j[0].IP != "" {
		t.Fatalf("records %+v", j)
	}
	if j := a.st.findJoins("", 10); len(j) != 2 {
		t.Fatalf("all records %+v", j)
	}

	// kick the player's link: the room stays
	m := room(t, a.Snapshot(), hexid(idn)).Members[1]
	if r := post(t, a, hs.URL, "/api/kick", `{"pair":"`+m.Pair+`","name":"치터"}`); !r.OK {
		t.Fatalf("kick %+v", r)
	}
	pc.SetReadDeadline(time.Now().Add(2 * time.Second))
	if _, err := pc.Read(make([]byte, 1)); err == nil {
		t.Fatal("the kicked player's link is still open")
	}
	time.Sleep(50 * time.Millisecond)
	if len(a.Snapshot().Rooms) != 1 {
		t.Fatal("kicking a player closed the room")
	}

	// close the room
	if r := post(t, a, hs.URL, "/api/close", `{"room":"`+hexid(idn)+`"}`); !r.OK {
		t.Fatalf("close %+v", r)
	}
	time.Sleep(100 * time.Millisecond)
	if len(a.Snapshot().Rooms) != 0 {
		t.Fatal("the room is still there")
	}

	// ban the port id for a day (not its address): kept on disk, its requests are refused, another port id from the
	// same address still gets in
	if r := post(t, a, hs.URL, "/api/ban", `{"id":"`+cheater+`","name":"치터","reason":"치트","days":1}`); !r.OK {
		t.Fatalf("ban %+v", r)
	}
	if b := a.st.banList(); len(b) != 1 || b[0].Until == nil || b[0].Reason != "치트" || b[0].ID != cheater {
		t.Fatalf("bans %+v", b)
	}
	c := dial(t, s)
	send(t, c, tList, headerAs(2, 0, 2))
	if e := expect(t, c, tError); binary.LittleEndian.Uint32(e) != errBanned {
		t.Fatal("a banned port id got an answer")
	}
	c = dial(t, s)
	send(t, c, tList, header(2, 0))
	expect(t, c, tHubs)
	a2, _ := NewAdmin(NewServer(DefaultConfig()), dir, "test")
	if !a2.st.banned("", cheater) {
		t.Fatal("the ban was not kept")
	}
	// an expired ban lets it in again
	past := time.Now().Add(-time.Minute)
	a2.st.ban(Ban{ID: "ZZZZ-ZZZZ", At: past, Until: &past})
	if a2.st.banned("", "ZZZZ-ZZZZ") {
		t.Fatal("an expired ban still holds")
	}

	// unban
	if r := post(t, a, hs.URL, "/api/unban", `{"id":"`+cheater+`"}`); !r.OK {
		t.Fatalf("unban %+v", r)
	}
	c = dial(t, s)
	send(t, c, tList, headerAs(2, 0, 2))
	expect(t, c, tHubs)

	// a ban of the address too: a new port id from it is refused at once
	nh := a.st.netOf("127.0.0.1")
	if r := post(t, a, hs.URL, "/api/ban", `{"id":"`+cheater+`","net":"`+nh+`","name":"치터","days":1}`); !r.OK {
		t.Fatalf("ban with the address %+v", r)
	}
	c = dial(t, s)
	send(t, c, tList, headerAs(2, 0, 9))
	c.SetReadDeadline(time.Now().Add(2 * time.Second))
	if _, err := c.Read(make([]byte, 1)); err == nil {
		t.Fatal("a banned address got an answer")
	}
	if r := post(t, a, hs.URL, "/api/unban", `{"id":"`+cheater+`"}`); !r.OK {
		t.Fatalf("unban 2 %+v", r)
	}
	c = dial(t, s)
	send(t, c, tList, header(2, 0))
	expect(t, c, tHubs)
	acts := a.st.actionList(10)
	kinds := []string{}
	for _, x := range acts {
		kinds = append(kinds, x.Kind)
	}
	if strings.Join(kinds, ",") != "unban,ban,unban,ban,close,kick" {
		t.Fatalf("actions %v", kinds)
	}

	// a POST from another page is refused
	req, _ := http.NewRequest("POST", hs.URL+"/api/unban", strings.NewReader(`{"id":"AAAA-AAAA"}`))
	req.Header.Set("Authorization", "Bearer "+a.token)
	req.Header.Set("Origin", "http://evil.example")
	resp, _ := http.DefaultClient.Do(req)
	resp.Body.Close()
	if resp.StatusCode != http.StatusForbidden {
		t.Fatalf("cross-origin POST: %d", resp.StatusCode)
	}
}

// Files from before (bans, records and actions with IP addresses) are rewritten with the addresses' hashes, and a ban
// of an address still holds.
func TestStoreForgetsAddresses(t *testing.T) {
	dir := t.TempDir()
	now := time.Now()
	day := joinFile(dir, now)
	os.WriteFile(filepath.Join(dir, "bans.json"), []byte(`[{"ip":"10.1.2.3","name":"옛날","at":"`+now.Format(time.RFC3339)+`"}]`), 0600)
	os.WriteFile(day, []byte(`{"at":"`+now.Format(time.RFC3339)+`","name":"손님","ip":"10.1.2.3","room":"r","host":"h","role":"player"}`+"\n"), 0600)
	os.WriteFile(filepath.Join(dir, "actions.log"), []byte(`{"at":"`+now.Format(time.RFC3339)+`","kind":"ban","target":"10.1.2.3"}`+"\n"), 0600)
	st := openStore(dir)
	for _, f := range []string{"bans.json", filepath.Base(day), "actions.log"} {
		b, _ := os.ReadFile(filepath.Join(dir, f))
		if strings.Contains(string(b), "10.1.2.3") {
			t.Fatalf("%s still has the address: %s", f, b)
		}
	}
	nh := st.netOf("10.1.2.3")
	if !st.banned(nh, "") || st.banned(st.netOf("10.1.2.4"), "") {
		t.Fatal("the old ban by address")
	}
	if j := st.findJoins("손님", 10); len(j) != 1 || j[0].Net != nh || j[0].IP != "" {
		t.Fatalf("old record %+v", j)
	}
	if st2 := openStore(dir); st2.netOf("10.1.2.3") != nh {
		t.Fatal("the key was not kept")
	}
}
