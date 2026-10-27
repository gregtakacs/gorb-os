package esprom

import "testing"

// crashMarker is the console voice between a pushed command and its ack: the
// bench failure of 2026-10-27 was a firmware that rebooted on every PROV with a
// timezone (an NVS write on the PSRAM-stack reader), and the hub could only
// report "no ack" while the console streamed the abort. The marker set is the
// receipts; the exclusion set is what keeps a commanded, honest reboot (MODE,
// RESET) and ordinary boot chatter from reading as a crash.

func TestCrashMarkerRecognisesThePanicVoices(t *testing.T) {
	crashy := []string{
		`assert failed: spi_flash_disable_interrupts_caches_and_other_cpu cache_utils.c:159 (esp_task_stack_is_sane_cache_disabled())`,
		`Guru Meditation Error: Core  1 panic'ed (Abort). Executing program`,
		`abort() was called at PC 0x420a12b6 on core 0`,
		`Kernel panic at 0x403760c9: 14 register files`,
		`rst:0xc (RTC_SW_CPU_RST),boot:0x2b (SPI_FAST_FLASH_BOOT)`,
	}
	for _, l := range crashy {
		if got := crashMarker(l); got == "" {
			t.Errorf("crashMarker(%q) = empty, want the line quoted back", l)
		}
	}
}

func TestCrashMarkerIgnoresTheHealthyConsole(t *testing.T) {
	quiet := []string{
		`I (7233) orb_net: serial door armed: PROV|NAME|SCAN|MODE|RESET|PING on the console`,
		`I (10069) orb_net: PROVSCAN-END 4`,
		`I (7224) orb_net: SoftAP up — join it and open the URL in the log above`,
		`I (7234) orb_net: PROV-ACK`,
		`I (1165) main: gorb-os running — knob: turn swaps apps, push opens, hold returns to the menu`,
		// The word "crash" in ordinary prose is nothing: the markers are IDF's
		// own literal voices. rst:0x is deliberately in the OTHER set — after a
		// pushed command, any reboot banner is a board that rebooted under us,
		// which is exactly the silence-with-no-ack the caller must not eat plain.
		`some line mentioning a crash of the test harness`,
	}
	for _, l := range quiet {
		if got := crashMarker(l); got != "" {
			t.Errorf("crashMarker(%q) = %q, want empty", l, got)
		}
	}
}

func TestCrashMarkerQuotesAtMostALine(t *testing.T) {
	long := "assert failed: " + string(make([]byte, 400))
	if got := crashMarker(long); len(got) > 140 {
		t.Errorf("crashMarker quoted %d bytes of a screaming console; the cap is 140", len(got))
	}
}
