package main

import (
	"flag"
	"log"
	"os"
	"runtime/debug"
)

// version: the commit this was built from (go build records it when the source is a git checkout).
func version() string {
	if bi, ok := debug.ReadBuildInfo(); ok {
		rev, dirty := "", ""
		for _, kv := range bi.Settings {
			switch kv.Key {
			case "vcs.revision":
				rev = kv.Value
			case "vcs.modified":
				if kv.Value == "true" {
					dirty = "+"
				}
			}
		}
		if len(rev) > 7 {
			return rev[:7] + dirty
		}
	}
	return "dev"
}

func main() {
	cfg := DefaultConfig()
	flag.StringVar(&cfg.Addr, "addr", cfg.Addr, "address and port to listen on (TCP and UDP)")
	flag.IntVar(&cfg.MaxConns, "max-conns", cfg.MaxConns, "connections at once")
	flag.IntVar(&cfg.MaxConnsPerIP, "max-conns-per-ip", cfg.MaxConnsPerIP, "connections at once from one IP address")
	flag.IntVar(&cfg.MaxHubs, "max-hubs", cfg.MaxHubs, "hubs at once")
	flag.IntVar(&cfg.MaxHubsPerIP, "max-hubs-per-ip", cfg.MaxHubsPerIP, "hubs at once from one IP address")
	flag.IntVar(&cfg.ListMax, "list-max", cfg.ListMax, "hubs in one search answer (a random choice when there are more)")
	flag.Float64Var(&cfg.Bandwidth, "bandwidth", cfg.Bandwidth, "bytes a second per player link and direction")
	flag.BoolVar(&cfg.Verbose, "v", false, "log hubs and joins (never IP addresses)")
	state := os.Getenv("STATE_DIRECTORY") // (systemd's StateDirectory=)
	if state == "" {
		state = "state"
	}
	adminAddr := flag.String("admin", "127.0.0.1:39520", "the admin view's address (loopback only; \"\": off)")
	flag.StringVar(&state, "state", state, "directory for the admin token (and, later, bans and records)")
	flag.Parse()
	s := NewServer(cfg)
	if err := s.Listen(); err != nil {
		log.Fatal(err)
	}
	if *adminAddr != "" {
		a, err := NewAdmin(s, state, version())
		if err != nil {
			log.Fatalf("admin view: %v", err)
		}
		go func() { log.Fatalf("admin view: %v", a.ListenAndServe(*adminAddr)) }()
		log.Printf("admin view on %s (through an SSH tunnel; token in %s)", *adminAddr, state)
	}
	log.Printf("MHGURecomp hub server on %s (TCP and UDP)", s.Addr())
	log.Fatal(s.Serve())
}
