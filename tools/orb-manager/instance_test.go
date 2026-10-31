package main

import (
	"os"
	"path/filepath"
	"testing"
)

// One hub, ever: a second claim in the same place is refused and told where the
// first one is, and the claim frees the moment its holder lets go.
func TestOnlyOneHubHoldsTheClaim(t *testing.T) {
	dir := t.TempDir()
	first, _, held, err := claimInstance(dir)
	if err != nil || !held {
		t.Fatalf("the first hub could not claim: held=%v err=%v", held, err)
	}

	// Still starting: no address announced yet, and the refusal says so.
	_, other, held, err := claimInstance(dir)
	if err != nil {
		t.Fatal(err)
	}
	if held {
		t.Fatal("a second hub claimed while the first held the lock")
	}
	if other.URL != "" {
		t.Fatalf("a hub that has not bound yet was announced at %q", other.URL)
	}

	first.announce("http://127.0.0.1:3779")
	_, other, held, _ = claimInstance(dir)
	if held {
		t.Fatal("a second hub claimed after the first announced")
	}
	if other.URL != "http://127.0.0.1:3779" || other.PID != os.Getpid() {
		t.Fatalf("the second launch read %+v; wanted the first hub's address and pid", other)
	}

	first.release()
	again, _, held, err := claimInstance(dir)
	if err != nil || !held {
		t.Fatalf("the claim did not free with its holder: held=%v err=%v", held, err)
	}
	defer again.release()
	if _, err := os.Stat(filepath.Join(dir, instanceInfoName)); !os.IsNotExist(err) {
		t.Fatal("the new holder inherited the old hub's address; a second launch would be sent to a page nobody serves")
	}
}
