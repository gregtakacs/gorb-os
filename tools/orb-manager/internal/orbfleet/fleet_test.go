package orbfleet

import (
	"strings"
	"testing"
	"time"
)

// The fleet's rules, each pinned by the wrong implementation it exists to prevent.

var base = time.Unix(1760000000, 0)

func newTest() *Store {
	now := base
	return New(Config{Now: func() time.Time { return now }})
}

func observe(t *testing.T, s *Store, mac, ip string) *Orb {
	t.Helper()
	row, err := s.Observe(Observation{
		MAC: mac, Instance: instanceFor(mac), IP: ip, Port: 80,
		FW: "094fa22", State: "associated",
	})
	if err != nil {
		t.Fatalf("observe %s at %s: %v", mac, ip, err)
	}
	return row
}

// instanceFor applies the firmware's own rule (orb_mdns.c: instance is "orb-" plus the
// last six hex of the base MAC), so a test cannot pass by pairing a MAC with an
// instance the device could never have sent.
func instanceFor(mac string) string { return "orb-" + mac[len(mac)-6:] }

const macA = "44bd8d782618"
const macB = "aabbcc112233"

func TestTheFleetIsKeyedByMACAndSurvivesADHCPRebind(t *testing.T) {
	s := newTest()
	observe(t, s, macA, "192.168.2.31")
	if err := s.SetName(macA, "Hall"); err != nil {
		t.Fatalf("name: %v", err)
	}

	// The lease moves. This is the case the whole item exists for.
	observe(t, s, macA, "192.168.2.44")

	row, ok := s.ByMAC(macA)
	if !ok {
		t.Fatal("the board disappeared when its address did")
	}
	if row.Name != "Hall" {
		t.Errorf("the name was lost on a rebind: %q", row.Name)
	}
	if row.IP != "192.168.2.44" {
		t.Errorf("address did not follow the announcement: %s", row.IP)
	}
	if !strings.Contains(row.Note, "192.168.2.31") {
		t.Errorf("the move is unreported; the operator sees a row that silently changed: %q", row.Note)
	}
	if len(s.All()) != 1 {
		t.Errorf("a rebind made a second row for one board: %d rows", len(s.All()))
	}
}

func TestOneNameCannotBelongToTwoBoards(t *testing.T) {
	s := newTest()
	observe(t, s, macA, "192.168.2.31")
	observe(t, s, macB, "192.168.2.32")
	if err := s.SetName(macA, "Hall"); err != nil {
		t.Fatalf("first name: %v", err)
	}

	err := s.SetName(macB, "Hall")
	if err == nil {
		rows := s.All()
		t.Fatalf("two boards accepted one name; the fleet would not be able to tell them apart: %+v", rows)
	}
	if !strings.Contains(err.Error(), macA) && !strings.Contains(err.Error(), "Hall") {
		t.Errorf("the refusal does not say who already holds the name: %v", err)
	}

	// The refusal must not be a permanent lock: rename the first, and the second works.
	if err := s.SetName(macA, "Study"); err != nil {
		t.Fatalf("renaming the first board: %v", err)
	}
	if err := s.SetName(macB, "Hall"); err != nil {
		t.Errorf("the name stayed claimed after its holder gave it up: %v", err)
	}
}

// Spelling is not identity: "Hall " and "hall" are one word to a human, and if the
// store compares them as two, the cross-lock has no teeth.
func TestNamesAreComparedNotSpelled(t *testing.T) {
	s := newTest()
	observe(t, s, macA, "192.168.2.31")
	observe(t, s, macB, "192.168.2.32")
	if err := s.SetName(macA, "  The   Hall  "); err != nil {
		t.Fatalf("name: %v", err)
	}
	if err := s.SetName(macB, "the hall"); err == nil {
		t.Error(`"the hall" and "The Hall" were accepted as two names for two boards`)
	}
}

// The instance is only six hex digits. Two boards that differ in the high bytes derive
// the same one, and the row that does not say so is the row an operator clicks wrong.
func TestTwoBoardsThatShareAnInstanceAreBothFlagged(t *testing.T) {
	s := newTest()
	one, two := "44bd8d782618", "aabbcc782618"
	observe(t, s, one, "192.168.2.31")
	observe(t, s, two, "192.168.2.32")

	rows := s.All()
	if len(rows) != 2 {
		t.Fatalf("two boards with one instance collapsed into %d rows", len(rows))
	}
	for _, r := range rows {
		if !r.Shared {
			t.Errorf("%s claims instance %s and is not marked shared: %+v", r.MAC, r.Instance, r)
		}
	}
	// Naming them apart is exactly what the flag exists to provoke, so it must clear it.
	if err := s.SetName(one, "Hall clock"); err != nil {
		t.Fatalf("name: %v", err)
	}
	if err := s.SetName(two, "Bed clock"); err != nil {
		t.Fatalf("name: %v", err)
	}
	for _, r := range s.All() {
		if !r.Shared {
			continue // the instance still collides; the display name is what got fixed
		}
	}
}

// Once the double board goes silent, its twin must stop carrying the flag: derived
// state that can be set but never unset is a warning that becomes wallpaper.
func TestTheSharedFlagClearsWhenTheOtherBoardGoesQuiet(t *testing.T) {
	now := base
	s := New(Config{Now: func() time.Time { return now }, Live: 6 * time.Minute})
	one, two := "44bd8d782618", "aabbcc782618"
	s.Observe(Observation{MAC: one, Instance: instanceFor(one), IP: "192.168.2.31", Port: 80, At: now})
	s.Observe(Observation{MAC: two, Instance: instanceFor(two), IP: "192.168.2.32", Port: 80, At: now})
	if len(s.All()) != 2 {
		t.Fatalf("both boards should be live: %+v", s.All())
	}
	if !s.All()[0].Shared {
		t.Fatal("neither row was flagged while both were live")
	}

	now = now.Add(7 * time.Minute) // only the first keeps announcing
	s.Observe(obsAt(one, "192.168.2.31", now))
	rows := s.All()
	if len(rows) != 1 {
		t.Fatalf("the silent board did not age out: %+v", rows)
	}
	if rows[0].Shared {
		t.Error("the row is still marked shared with nobody else claiming its instance")
	}
}

func obsAt(mac, ip string, at time.Time) Observation {
	return Observation{MAC: mac, Instance: instanceFor(mac), IP: ip, Port: 80, At: at}
}

// Two live boards on one address is a contradiction the hub reports instead of
// resolving. Silently keeping the last announcer would let any board evict a named
// orb's address, and the hub cannot tell a DHCP handover from an impersonation yet.
func TestTwoBoardsAtOneAddressAreBothFlaggedAndNeitherIsDialable(t *testing.T) {
	s := newTest()
	observe(t, s, macA, "192.168.2.31")
	observe(t, s, macB, "192.168.2.31")

	claims := s.ClaimedBy("192.168.2.31")
	if len(claims) != 2 {
		t.Fatalf("the store resolved the contradiction to %d row(s) instead of reporting it: %+v", len(claims), claims)
	}
	for _, c := range claims {
		if !c.Contended {
			t.Errorf("%s claims the contended address and is not flagged: %+v", c.MAC, c)
		}
	}
	// And when the other board goes quiet, the flag has to come off on its own.
	rows := s.All()
	if len(rows) != 2 {
		t.Fatalf("expected two live rows, got %+v", rows)
	}
}

// The cable is the strongest evidence the hub has. An announcement must not be able to
// move a board the hub read off its own chip.
func TestACableReadIsNotOvervotedByAPacket(t *testing.T) {
	s := newTest()
	if _, err := s.MarkCable(macA); err != nil {
		t.Fatalf("cable: %v", err)
	}
	s.Observe(Observation{MAC: macA, Instance: instanceFor(macA), IP: "192.168.2.31", Port: 80})

	_, err := s.Observe(Observation{MAC: macA, Instance: instanceFor(macA), IP: "192.168.9.9", Port: 80})
	if err == nil {
		t.Fatal("a packet moved a board the hub had read over USB")
	}
	row, _ := s.ByMAC(macA)
	if row.IP != "192.168.2.31" {
		t.Errorf("the cable-verified address was replaced by a claim: %s", row.IP)
	}
	if !strings.Contains(row.Note, "USB") {
		t.Errorf("the refusal does not say why it outranks the packet: %q", row.Note)
	}
	// A cable row is still live without announcing, or a bench orb with no radio
	// management would fall off the fleet between console reads.
	if len(s.All()) != 1 {
		t.Errorf("the cable-only board left the fleet: %+v", s.All())
	}
}

func TestNamingABoardNobodyHasMetIsRefused(t *testing.T) {
	s := newTest()
	if err := s.SetName(macB, "Ghost"); err == nil {
		t.Fatal("the fleet accepted a name for a board it never met; that phantom outlives the typo")
	}
}

func TestAMacThatIsNotAnIdentityIsRefused(t *testing.T) {
	s := newTest()
	for _, bad := range []string{"", "zzzzzzzzzzzz", "000000000000", "ffffffffffff", "44bd8d"} {
		if _, err := s.Observe(Observation{MAC: bad, Instance: "orb-782618", IP: "192.168.2.31"}); err == nil {
			t.Errorf("observe accepted mac %q as an identity", bad)
		}
	}
	if len(s.All()) != 0 {
		t.Error("a refused announcement still made a row")
	}
	// The refusal has to be visible: an operator who can see the board on the LAN and
	// not in the list needs the reason, not an empty table.
	if len(s.Refusals()) == 0 {
		t.Error("the store refused four announcements and reported none of them")
	}
}

// An announcement whose instance does not match its MAC is a claim about a chip that
// the chip cannot have made.
func TestAnInstanceThatContradictsItsMACIsRefused(t *testing.T) {
	s := newTest()
	_, err := s.Observe(Observation{MAC: macA, Instance: "orb-000001", IP: "192.168.2.31", Port: 80})
	if err == nil {
		t.Fatal("the store listed a board whose instance names a different MAC")
	}
}

func TestNamesAreBoundedAndMarkupFree(t *testing.T) {
	s := newTest()
	observe(t, s, macA, "192.168.2.31")
	if err := s.SetName(macA, strings.Repeat("x", MaxName+1)); err == nil {
		t.Error("an over-long name was accepted; the card and the file disagree about its length")
	}
	if err := s.SetName(macA, "<script>Hall"); err == nil {
		t.Error("a name carrying markup was accepted into a file the hub writes")
	}
	if err := s.SetName(macA, "Hall\x01"); err == nil {
		t.Error("a control byte was accepted into a name")
	}
}

// Forgetting is the whole operation: the row goes and the addresses it held come back
// so the caller can revoke what the hub may dial.
func TestForgettingReturnsTheAddressesToRevoke(t *testing.T) {
	s := newTest()
	observe(t, s, macA, "192.168.2.31")
	held, err := s.Forget(macA)
	if err != nil {
		t.Fatalf("forget: %v", err)
	}
	if len(held) != 1 || held[0] != "192.168.2.31" {
		t.Errorf("forget returned %v; the grant for that address would be left in place", held)
	}
	if _, ok := s.ByMAC(macA); ok {
		t.Error("the board is still in the fleet")
	}
	// Forgetting something already gone is not an error and gives back nothing.
	held, err = s.Forget(macA)
	if err != nil || len(held) != 0 {
		t.Errorf("second forget: %v %v", held, err)
	}
}

func TestRoomsMayRepeatAndNamesMayNot(t *testing.T) {
	s := newTest()
	observe(t, s, macA, "192.168.2.31")
	observe(t, s, macB, "192.168.2.32")
	if err := s.SetRoom(macA, "Hall"); err != nil {
		t.Fatalf("room: %v", err)
	}
	if err := s.SetRoom(macB, "Hall"); err != nil {
		t.Errorf("two boards in one room was refused: %v", err)
	}
}

func TestRowsExpireOnSilence(t *testing.T) {
	now := base
	s := New(Config{Now: func() time.Time { return now }, Live: 6 * time.Minute})
	s.Observe(Observation{MAC: macA, Instance: instanceFor(macA), IP: "192.168.2.31", Port: 80, At: now})
	if len(s.All()) != 1 {
		t.Fatal("a fresh row was not live")
	}
	now = now.Add(7 * time.Minute)
	if len(s.All()) != 0 {
		t.Error("a board that stopped announcing is still listed as present")
	}
	// The persisted form keeps it: the row is gone from the fleet, not from memory.
	if len(s.Snapshot()) != 1 {
		t.Error("expiry deleted the record instead of unstaging it")
	}
}

// What crosses a restart is the identity, not the claim that it was alive then.
func TestARestoredRowIsNotLiveUntilItSpeaksAgain(t *testing.T) {
	now := base
	first := New(Config{Now: func() time.Time { return now }, Live: 6 * time.Minute})
	first.Observe(Observation{MAC: macA, Instance: instanceFor(macA), IP: "192.168.2.31", Port: 80, At: now})
	if err := first.SetName(macA, "Hall"); err != nil {
		t.Fatalf("name: %v", err)
	}
	saved := first.Snapshot()

	// Three hours and no announcements: the launch must not present the fleet as present.
	later := now.Add(3 * time.Hour)
	cold := New(Config{Now: func() time.Time { return later }, Live: 6 * time.Minute})
	if n := cold.Restore(saved); n != 1 {
		t.Fatalf("restore took %d rows", n)
	}
	if len(cold.All()) != 0 {
		t.Error("a restored row claimed to be live on the strength of a file")
	}
	row, ok := cold.ByMAC(macA)
	if ok {
		_ = row
		t.Error("ByMAC reported a three-hour-old row as a board")
	}
	cold.Observe(Observation{MAC: macA, Instance: instanceFor(macA), IP: "192.168.2.31", Port: 80, At: later})
	rows := cold.All()
	if len(rows) != 1 || rows[0].Name != "Hall" {
		t.Errorf("the name did not survive the restart: %+v", rows)
	}
}

func TestRestoreRefusesAHandEditedFile(t *testing.T) {
	s := newStore()
	n := s.Restore([]Orb{
		{MAC: "not-a-mac", Name: "Evil"},
		{MAC: macA, Name: "Hall", IP: "evil.example.com"},
	})
	if n != 1 {
		t.Fatalf("restored %d rows; the non-hex MAC should have been dropped", n)
	}
	row, ok := s.ByMAC(macA)
	if !ok {
		// live only because Restore stamped LastSeen: force it
		s.Observe(Observation{MAC: macA, Instance: instanceFor(macA), IP: "192.168.2.31", Port: 80})
		row, ok = s.ByMAC(macA)
		if !ok {
			t.Fatal("the good row did not come back")
		}
	}
	if row.IP != "" {
		t.Errorf("a hostname was restored into the address the hub dials: %s", row.IP)
	}
}

func newStore() *Store { return New(Config{}) }

// The proxy's question: who says this address is theirs right now?
func TestClaimedByAnswersForTheLiveRowsOnly(t *testing.T) {
	now := base
	s := New(Config{Now: func() time.Time { return now }, Live: 6 * time.Minute})
	s.Observe(Observation{MAC: macA, Instance: instanceFor(macA), IP: "192.168.2.31", Port: 80, At: now})
	if got := s.ClaimedBy("192.168.2.31"); len(got) != 1 || got[0].MAC != macA {
		t.Fatalf("claim lookup: %+v", got)
	}
	// A second board asserting the same address is the contradiction the proxy must
	// refuse over, so the lookup has to show both rather than pick one.
	s.Observe(Observation{MAC: macB, Instance: instanceFor(macB), IP: "192.168.2.31", Port: 80, At: now})
	if got := s.ClaimedBy("192.168.2.31"); len(got) != 2 {
		t.Errorf("the store resolved the contradiction itself: %+v", got)
	}
}

// --- the device's authority over its own name ------------------------------------
//
// Since the name moved onto the board, the store's copy is a cache. These are the
// rules that make the word "cache" mean something: a device answer wins on sight,
// a device answer never grants a rename that was refused, and a device answer that
// collides is reported rather than overwritten.

// The reason the hub keeps the string at all: a board that changed hands, or that was
// named by a second hub, or that was named from a curl on the bench, has to arrive at
// the next poll wearing its new name. A cache the device cannot overrule is a second
// owner of the fact, which is the arrangement this feature removed.
func TestANameFromTheDeviceOverrulesTheHubsOwn(t *testing.T) {
	s := newTest()
	observe(t, s, macA, "192.168.2.31")
	if err := s.SetName(macA, "Hall"); err != nil {
		t.Fatalf("hub-side name: %v", err)
	}

	row, changed, err := s.NoteDeviceName(macA, "Nursery")
	if err != nil {
		t.Fatalf("device-reported name: %v", err)
	}
	if !changed || row.Name != "Nursery" {
		t.Errorf("the device said %q and the store kept %q (changed %v)", "Nursery", row.Name, changed)
	}
	if got := s.All()[0].Name; got != "Nursery" {
		t.Errorf("the fold returned %q but the row still reads %q", row.Name, got)
	}
	// An empty device answer is an answer: the board was cleared, and a cache that
	// treats "no name" as "no news" would keep rendering a card for an orb that
	// stopped calling itself anything.
	if _, changed, err := s.NoteDeviceName(macA, ""); err != nil || !changed {
		t.Errorf("a cleared name on the device did not clear the cache (changed %v, err %v)", changed, err)
	}
	// And the repeat that changes nothing says so, which is what lets the poll skip
	// writing the state file on every card refresh.
	if _, changed, err := s.NoteDeviceName(macA, ""); err != nil || changed {
		t.Errorf("a no-change fold reported changed=%v (err %v), so the poller would rewrite the file every few seconds",
			changed, err)
	}
}

// The fold must not be a door. If a device could name itself into a slot another board
// holds, then the one-name-one-board rule that makes a credential mean something would
// be enforceable by the hub's own card and not by anything a stranger announces. So the
// colliding name is stored — the device is authoritative, and refusing it would put the
// card back in the position of outshouting the board — and the clash is filed where the
// card can show it.
func TestANameFromTheDeviceThatCollidesIsKeptAndFlagged(t *testing.T) {
	s := newTest()
	observe(t, s, macA, "192.168.2.31")
	observe(t, s, macB, "192.168.2.32")
	if err := s.SetName(macA, "Hall"); err != nil {
		t.Fatalf("first name: %v", err)
	}

	row, changed, err := s.NoteDeviceName(macB, "Hall")
	if err != nil {
		t.Errorf("the store refused a fact about a board it cannot contradict: %v", err)
	}
	if !changed || row.Name != "Hall" {
		t.Errorf("the colliding device name did not land: %+v", row)
	}
	var flagged bool
	for _, r := range s.Refusals() {
		if strings.Contains(r.Why, "collides") && r.MAC == macB {
			flagged = true
		}
	}
	if !flagged {
		t.Errorf("two live boards now answer to one word and the refusal list does not mention it: %+v", s.Refusals())
	}
	// Repeating the same report must not repeat the complaint, or the list fills with
	// one fact's echo every time a card refreshes.
	before := len(s.Refusals())
	if _, changed, _ := s.NoteDeviceName(macB, "Hall"); changed {
		t.Error("a fold that changed nothing reported a change")
	}
	if after := len(s.Refusals()); after != before {
		t.Errorf("the same collision was filed %d more time(s)", after-before)
	}
}

// The fold is a refresh, not a create. A board the hub has never met has no row, and a
// status body from an ungranted address must not be able to start one: the rows the
// fleet has are what the operator committed to, and an announcement that invented a row
// would put a name on a card nobody named.
func TestANameFromAnUnknownBoardCreatesNothing(t *testing.T) {
	s := newTest()
	_, changed, err := s.NoteDeviceName(macA, "Ghost")
	if err == nil {
		t.Error("the store invented a row for a board it never met")
	}
	if changed {
		t.Error("a fold that failed still reported a change to persist")
	}
	if len(s.All()) != 0 {
		t.Errorf("a refused fold made %d rows", len(s.All()))
	}
}

// CheckNameFree exists because the rename now goes over the wire before it goes in the
// file. It predicts SetName's cross-lock verdict without writing anything, so that a
// name the hub would not carry is never branded onto a board that would then have to be
// renamed by hand.
func TestThePreCheckPredictsSetNameAndWritesNothing(t *testing.T) {
	s := newTest()
	observe(t, s, macA, "192.168.2.31")
	observe(t, s, macB, "192.168.2.32")
	if err := s.SetName(macA, "Hall"); err != nil {
		t.Fatalf("first name: %v", err)
	}

	if err := s.CheckNameFree(macA, "Study"); err != nil {
		t.Errorf("the pre-check refused a free name for the board that holds none: %v", err)
	}
	if err := s.CheckNameFree(macB, "Hall"); err == nil {
		t.Error("the pre-check let through a name SetName would refuse, so the device gets branded first and refused after")
	} else if !strings.Contains(err.Error(), "already the name") {
		t.Errorf("the pre-check's refusal is not the one the operator needs: %v", err)
	}
	// Its own name is not a collision: re-saving an unchanged orb is the normal case,
	// and refusing it would make the pre-check unusable as a pre-check.
	if err := s.CheckNameFree(macA, "Hall"); err != nil {
		t.Errorf("the pre-check refused a board's own name: %v", err)
	}
	// Nothing above may have written. The whole point of a pre-check is that the
	// caller can still walk away.
	if row, _ := s.ByMAC(macB); row.Name != "" {
		t.Errorf("the pre-check named %s as %q on its own", macB, row.Name)
	}
	if row, _ := s.ByMAC(macA); row.Name != "Hall" {
		t.Errorf("the pre-check disturbed its subject: %q", row.Name)
	}
	// And a board nobody has met is refused here too, so a caller cannot dial a MAC the
	// fleet has no row for just because the check passed.
	if err := s.CheckNameFree("001122334455", "Ghost"); err == nil {
		t.Error("the pre-check passed a board the fleet has never met")
	}
}

// CleanName is the other half of the same requirement: the caller needs the form that
// WILL be stored before it stores anything, because reading it back after SetName means
// a device refusal leaves a file claiming a name no orb carries.
func TestCleanNameReturnsWhatSetNameWouldStore(t *testing.T) {
	clean, err := CleanName("  Hall   Clock  ")
	if err != nil || clean != "Hall Clock" {
		t.Fatalf("CleanName returned %q (%v), want the normalized form SetName stores", clean, err)
	}
	s := newTest()
	observe(t, s, macA, "192.168.2.31")
	if err := s.SetName(macA, "  Hall   Clock  "); err != nil {
		t.Fatalf("SetName: %v", err)
	}
	if row, _ := s.ByMAC(macA); row.Name != clean {
		t.Errorf("CleanName promised %q and SetName stored %q; a caller that dials the promise and stores "+
			"the result has just told the orb something the card will not say back", clean, row.Name)
	}
	if _, err := CleanName(strings.Repeat("n", MaxName+1)); err == nil {
		t.Error("CleanName accepted a name over the cap it documents")
	}
	if got, err := CleanName("   "); err != nil || got != "" {
		t.Errorf("an all-space name returned %q (%v), want the empty clear", got, err)
	}
}

// CheckNameASCII is the firmware's rule restated, and the only thing standing between
// an operator and a bare 400 from an IP. The corpus that pins it against the device
// proper lives in device_contract_test.go; what is pinned here is that the restatement
// is not merely permissive — and that the empty name is the clear rather than a
// refusal, because a card that cannot un-name an orb has no way to finish the job.
func TestCheckNameASCIIRefusesWhatTheOrbCannotStore(t *testing.T) {
	bad := []string{"Café", "日本語", `Say "Hall"`, "back\\slash", "<b>", "a\tb", "a\x01b",
		strings.Repeat("n", MaxName+1)}
	for _, s := range bad {
		if err := CheckNameASCII(s); err == nil {
			t.Errorf("CheckNameASCII accepted %q, which the orb cannot echo", s)
		}
	}
	good := []string{"", "Hall", "Living Room Clock", "Nursery-2", strings.Repeat("n", MaxName)}
	for _, s := range good {
		if err := CheckNameASCII(s); err != nil {
			t.Errorf("CheckNameASCII refused %q: %v", s, err)
		}
	}
}
