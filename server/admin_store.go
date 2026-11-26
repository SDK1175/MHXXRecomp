// What the admin view keeps on disk, in the state directory (docs/hub-admin.md, "개인정보"):
//
//	bans.json            the bans (a port id and/or a hashed address, the name it had, why, until when)
//	joins-YYYY-MM-DD.jsonl who came in (port id, hashed address, name, room): kept 3 days, then deleted
//	actions.log          what the admin did: kept a year
//	ip.key               the secret the addresses are hashed with (netOf)
//
// No IP address is written: files from before (with addresses) are rewritten with their hashes when opened.
// Nothing here goes to the repository; the files are written 0600.
package main

import (
	"bufio"
	"crypto/hmac"
	"crypto/rand"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"net"
	"os"
	"path/filepath"
	"sort"
	"strings"
	"sync"
	"time"
)

const (
	joinKeep   = 3 * 24 * time.Hour
	actionKeep = 365 * 24 * time.Hour
)

type Ban struct {
	ID     string     `json:"id,omitempty"`   // port id
	Net    string     `json:"net,omitempty"`  // hashed address: connections from it are refused too
	IP     string     `json:"ip,omitempty"`   // (only in bans from before: openStore hashes it)
	Name   string     `json:"name,omitempty"` // the name it had when banned
	Reason string     `json:"reason,omitempty"`
	At     time.Time  `json:"at"`
	Until  *time.Time `json:"until,omitempty"` // none: for good
}

type Action struct {
	At     time.Time `json:"at"`
	Kind   string    `json:"kind"` // kick, close, ban, unban
	Target string    `json:"target"`
	Name   string    `json:"name,omitempty"`
	Reason string    `json:"reason,omitempty"`
	Device string    `json:"device,omitempty"`
}

type store struct {
	dir string
	key []byte // ip.key

	banMu sync.Mutex
	bans  map[string]Ban

	recMu   sync.Mutex
	joins   []JoinRecord
	actions []Action
}

func openStore(dir string) *store {
	st := &store{dir: dir, bans: map[string]Ban{}}
	path := filepath.Join(dir, "ip.key")
	if b, err := os.ReadFile(path); err == nil && len(b) >= 32 {
		st.key = b[:32]
	} else {
		st.key = make([]byte, 32)
		rand.Read(st.key)
		writeFileAtomic(path, st.key)
	}
	if b, err := os.ReadFile(filepath.Join(dir, "bans.json")); err == nil {
		var list []Ban
		old := false
		if json.Unmarshal(b, &list) == nil {
			for _, x := range list {
				if x.IP != "" {
					x.Net, x.IP, old = st.netOf(x.IP), "", true
				}
				st.bans[banKey(x)] = x
			}
		}
		if old {
			st.saveBansLocked()
		}
	}
	st.loadJoins()
	st.loadActions()
	return st
}

// netOf: an address's keyed hash, the same for the same address (and secret): two connections can be matched, the
// address can't be read back.
func (st *store) netOf(ip string) string {
	m := hmac.New(sha256.New, st.key)
	m.Write([]byte(ip))
	return hex.EncodeToString(m.Sum(nil))[:12]
}

func banKey(b Ban) string {
	if b.ID != "" {
		return "id:" + b.ID
	}
	return "net:" + b.Net
}

func writeFileAtomic(path string, b []byte) error {
	tmp := path + ".part"
	if err := os.WriteFile(tmp, b, 0600); err != nil {
		return err
	}
	return os.Rename(tmp, path)
}

func appendLine(path string, v any) {
	b, err := json.Marshal(v)
	if err != nil {
		return
	}
	f, err := os.OpenFile(path, os.O_CREATE|os.O_WRONLY|os.O_APPEND, 0600)
	if err != nil {
		return
	}
	f.Write(append(b, '\n'))
	f.Close()
}

func readLines[T any](path string, keep func(T) bool) []T {
	f, err := os.Open(path)
	if err != nil {
		return nil
	}
	defer f.Close()
	var out []T
	sc := bufio.NewScanner(f)
	sc.Buffer(make([]byte, 64<<10), 1<<20)
	for sc.Scan() {
		var v T
		if json.Unmarshal(sc.Bytes(), &v) == nil && keep(v) {
			out = append(out, v)
		}
	}
	return out
}

// ---- bans

// banned: whether a connection from this hashed address, or of this port id, is refused now (either may be ""; expired
// bans are dropped here).
func (st *store) banned(nh, pid string) bool {
	st.banMu.Lock()
	defer st.banMu.Unlock()
	now, hit := time.Now(), false
	for k, b := range st.bans {
		if !(pid != "" && b.ID == pid || nh != "" && b.Net == nh) {
			continue
		}
		if b.Until != nil && now.After(*b.Until) {
			delete(st.bans, k)
			st.saveBansLocked()
			continue
		}
		hit = true
	}
	return hit
}

func (st *store) ban(b Ban) {
	st.banMu.Lock()
	st.bans[banKey(b)] = b
	st.saveBansLocked()
	st.banMu.Unlock()
}

// unban: the ban of this port id, or (id "") of this hashed address.
func (st *store) unban(id, nh string) bool {
	st.banMu.Lock()
	defer st.banMu.Unlock()
	k := banKey(Ban{ID: id, Net: nh})
	if _, ok := st.bans[k]; !ok || (id == "" && nh == "") {
		return false
	}
	delete(st.bans, k)
	st.saveBansLocked()
	return true
}

func (st *store) banList() []Ban {
	st.banMu.Lock()
	defer st.banMu.Unlock()
	now := time.Now()
	out := []Ban{}
	for _, b := range st.bans {
		if b.Until == nil || now.Before(*b.Until) {
			out = append(out, b)
		}
	}
	sort.Slice(out, func(i, j int) bool { return out[i].At.After(out[j].At) })
	return out
}

func (st *store) saveBansLocked() {
	list := make([]Ban, 0, len(st.bans))
	for _, b := range st.bans {
		list = append(list, b)
	}
	b, _ := json.MarshalIndent(list, "", "  ")
	writeFileAtomic(filepath.Join(st.dir, "bans.json"), b)
}

// ---- who came in (3 days)

func joinFile(dir string, t time.Time) string {
	return filepath.Join(dir, "joins-"+t.Format("2006-01-02")+".jsonl")
}

func (st *store) loadJoins() {
	cut := time.Now().Add(-joinKeep)
	files, _ := filepath.Glob(filepath.Join(st.dir, "joins-*.jsonl"))
	sort.Strings(files)
	for _, f := range files {
		js := readLines(f, func(j JoinRecord) bool { return true })
		old := false
		for i := range js {
			if js[i].IP != "" {
				js[i].Net, js[i].IP, old = st.netOf(js[i].IP), "", true
			}
		}
		if old { // (a file from before: its addresses hashed)
			var sb strings.Builder
			for _, j := range js {
				b, _ := json.Marshal(j)
				sb.Write(b)
				sb.WriteByte('\n')
			}
			writeFileAtomic(f, []byte(sb.String()))
		}
		for _, j := range js {
			if j.At.After(cut) {
				st.joins = append(st.joins, j)
			}
		}
	}
	st.pruneJoinFiles()
}

// pruneJoinFiles: a day's file goes once its last possible record is older than 3 days.
func (st *store) pruneJoinFiles() {
	files, _ := filepath.Glob(filepath.Join(st.dir, "joins-*.jsonl"))
	for _, f := range files {
		day, err := time.ParseInLocation("2006-01-02", strings.TrimSuffix(strings.TrimPrefix(filepath.Base(f), "joins-"), ".jsonl"), time.Local)
		if err == nil && time.Since(day.Add(24*time.Hour)) > joinKeep {
			os.Remove(f)
		}
	}
}

// record: called with the server's lock held, so it only appends.
func (st *store) record(j JoinRecord) {
	st.recMu.Lock()
	st.joins = append(st.joins, j)
	st.recMu.Unlock()
	appendLine(joinFile(st.dir, j.At), j)
}

func (st *store) findJoins(q string, limit int) []JoinRecord {
	st.recMu.Lock()
	defer st.recMu.Unlock()
	cut := time.Now().Add(-joinKeep)
	q = strings.ToLower(strings.TrimSpace(q))
	out := []JoinRecord{}
	for i := len(st.joins) - 1; i >= 0 && len(out) < limit; i-- {
		j := st.joins[i]
		if j.At.Before(cut) {
			break
		}
		if q == "" || strings.Contains(strings.ToLower(j.Name), q) || strings.Contains(strings.ToLower(j.ID), q) ||
			strings.Contains(j.Net, q) || strings.Contains(strings.ToLower(j.Host), q) {
			out = append(out, j)
		}
	}
	return out
}

// ---- what the admin did (a year)

func (st *store) loadActions() {
	path := filepath.Join(st.dir, "actions.log")
	cut := time.Now().Add(-actionKeep)
	all := readLines(path, func(Action) bool { return true })
	kept := readLines(path, func(a Action) bool { return a.At.After(cut) })
	old := false
	for i := range kept { // (records from before named an address: its hash now)
		if net.ParseIP(kept[i].Target) != nil {
			kept[i].Target, old = "net:"+st.netOf(kept[i].Target), true
		}
	}
	if len(kept) != len(all) || old {
		var sb strings.Builder
		for _, a := range kept {
			b, _ := json.Marshal(a)
			sb.Write(b)
			sb.WriteByte('\n')
		}
		writeFileAtomic(path, []byte(sb.String()))
	}
	st.actions = kept
}

func (st *store) act(a Action) {
	st.recMu.Lock()
	st.actions = append(st.actions, a)
	st.recMu.Unlock()
	appendLine(filepath.Join(st.dir, "actions.log"), a)
}

func (st *store) actionList(limit int) []Action {
	st.recMu.Lock()
	defer st.recMu.Unlock()
	out := []Action{}
	for i := len(st.actions) - 1; i >= 0 && len(out) < limit; i-- {
		out = append(out, st.actions[i])
	}
	return out
}

// janitor: the 3 days and the expired bans, hourly.
func (st *store) janitor() {
	for range time.Tick(time.Hour) {
		cut := time.Now().Add(-joinKeep)
		st.recMu.Lock()
		i := 0
		for i < len(st.joins) && st.joins[i].At.Before(cut) {
			i++
		}
		st.joins = append([]JoinRecord(nil), st.joins[i:]...)
		st.recMu.Unlock()
		st.pruneJoinFiles()
		st.banList() // (drops nothing itself; banned() does when asked)
	}
}
