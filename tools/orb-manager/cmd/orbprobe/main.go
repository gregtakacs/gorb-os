// Command orbprobe answers "what is on the end of this cable" from the terminal.
// It runs the same probe the hub's Identify button runs — including the reboot it
// costs — so a bench can check an orb, or an agent can reproduce what the card
// shows, without driving the web UI.
package main

import (
	"flag"
	"fmt"
	"os"

	"github.com/gregtakacs/gorb-os/tools/orb-manager/internal/esprom"
	"github.com/gregtakacs/gorb-os/tools/orb-manager/internal/orbbind"
)

func main() {
	port := flag.String("port", "", "serial device to identify (this restarts the orb)")
	flag.Parse()
	if *port == "" {
		fmt.Fprintln(os.Stderr, "orbprobe: -port is required; note this reboots the orb")
		os.Exit(2)
	}
	// The same lock the hub takes: two resets on one wire is how a flash dies.
	lock, err := orbbind.LockPort(*port)
	if err != nil {
		fmt.Fprintln(os.Stderr, "orbprobe:", err)
		os.Exit(1)
	}
	defer lock.Release()

	info, err := esprom.ProbeOrb(*port)
	if err != nil {
		fmt.Fprintln(os.Stderr, "orbprobe:", err)
		os.Exit(1)
	}
	if !info.IsOrb {
		fmt.Printf("%s: not an orb - %s\n", info.Port, info.Reason)
		return
	}
	fmt.Printf("%s: gorb-os %s\n  mac        %s\n  built      %s\n  elf sha256 %s\n  idf        %s\n",
		info.Port, info.Version, info.MAC, info.CompileTime, info.ElfSHA256, info.IDF)
	if info.IP != "" {
		fmt.Printf("  lan        %s\n", info.IP)
	}
}
