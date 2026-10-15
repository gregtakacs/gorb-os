package main

import (
	"encoding/json"
	"testing"

	"github.com/gregtakacs/gorb-os/tools/orb-manager/internal/esprom"
)

// flashProgressSender is the one progress shape every streaming door emits, and the bar
// reads the byte-weighted percentage out of it. These cases pin the two properties the UI
// depends on: the figure is cumulative across images, so a small image late in the plan
// cannot make the bar jump backward, and it lands on exactly 100 at the end.
func TestProgressWeightsTheWholePlan(t *testing.T) {
	big := make([]byte, 4000)
	small := make([]byte, 40)
	p := esprom.FlashPlan{Images: []esprom.Image{
		{Offset: 0x0, Name: "bootloader.bin", Data: big},
		{Offset: 0x8000, Name: "otadata.bin", Data: small},
		{Offset: 0x9000, Name: "app.bin", Data: big[:1960]},
	}}
	var last []map[string]any
	send := func(kind, msg string) {
		if kind != "progress" {
			return
		}
		var ev map[string]any
		if err := json.Unmarshal([]byte(msg), &ev); err != nil {
			t.Fatalf("progress is not JSON: %v", err)
		}
		last = append(last, ev)
	}
	w := flashProgressSender(send, p)

	// Image 2, halfway: the bar must sit past the first two images' bytes and short of
	// the total, which a per-image percentage would have called 50%.
	w(2, 3, 0x9000, 980, 1960)
	// The last byte of the last image: exactly 100, and never more.
	w(2, 3, 0x9000, 1960, 1960)

	if len(last) != 2 {
		t.Fatalf("got %d progress events", len(last))
	}
	first, second := last[0]["pct"].(float64), last[1]["pct"].(float64)
	// before[2] is 4040 B and the plan is 6000 B, so half of image 2 is 5020/6000 = 83%.
	if first < 82 || first > 84 {
		t.Errorf("mid-image-2 reported %v%%, want 83%% (4040+980 of 6000): the weights are "+
			"not cumulative over the plan", first)
	}
	if second != 100 {
		t.Errorf("the plan finished at %v%%, not 100", second)
	}
	if got := last[0]["bytes"].(float64); got != 4000+40+1960 {
		t.Errorf("total bytes %v: the figure the bar quotes must be the plan it is writing", got)
	}
	// A bar the size of one image would report 50% at the same moment. The point of the
	// weight is that the number the operator watches is the wait they are in.
	if perImage := 980 * 100 / 1960; perImage == int(first) {
		t.Errorf("the cumulative figure %v%% is the per-image figure: nothing was weighted", first)
	}
}

// A plan with nothing in it must not divide by zero, and must not claim progress it did
// not make — the empty case is the one a corrupt bundle produces.
func TestProgressOnAnEmptyPlanSaysZero(t *testing.T) {
	var ev map[string]any
	w := flashProgressSender(func(kind, msg string) {
		_ = json.Unmarshal([]byte(msg), &ev)
	}, esprom.FlashPlan{})
	w(0, 1, 0, 0, 0)
	if ev["pct"].(float64) != 0 {
		t.Errorf("an empty plan reported %v%%", ev["pct"])
	}
}
