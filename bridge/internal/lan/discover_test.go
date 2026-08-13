package lan

import (
	"net"
	"testing"
	"time"
)

func TestNormalizeMAC(t *testing.T) {
	tests := []struct {
		in   string
		want string
	}{
		{"18:50:73:12:6F:8D", "185073126f8d"},
		{"18-50-73-12-6f-8d", "185073126f8d"},
		{"185073126F8D", "185073126f8d"},
		{"", ""},
		{"not a mac", "aac"}, // only hex digits survive, which the caller length-checks
	}

	for _, tt := range tests {
		if got := NormalizeMAC(tt.in); got != tt.want {
			t.Errorf("NormalizeMAC(%q) = %q, want %q", tt.in, got, tt.want)
		}
	}
}

func TestNormalizeMACMatchesAcrossSeparators(t *testing.T) {
	// The cloud uses colons and Windows uses hyphens for the same address.
	if NormalizeMAC("E4:AA:EC:87:53:54") != NormalizeMAC("e4-aa-ec-87-53-54") {
		t.Error("colon and hyphen forms of one address do not compare equal")
	}
}

func TestBroadcastOf(t *testing.T) {
	tests := []struct {
		cidr string
		want string
	}{
		{"192.168.1.11/24", "192.168.1.255"},
		{"10.0.0.5/8", "10.255.255.255"},
		{"172.16.4.9/20", "172.16.15.255"},
		{"192.168.56.1/24", "192.168.56.255"},
	}

	for _, tt := range tests {
		ip, n, err := net.ParseCIDR(tt.cidr)
		if err != nil {
			t.Fatalf("bad test CIDR %s: %v", tt.cidr, err)
		}
		n.IP = ip

		got := broadcastOf(n)
		if got == nil || got.String() != tt.want {
			t.Errorf("broadcastOf(%s) = %v, want %s", tt.cidr, got, tt.want)
		}
	}
}

func TestBroadcastForUnknownSubnet(t *testing.T) {
	// An address on no locally attached network has no directed broadcast, and
	// guessing one would spray the request at an unrelated subnet.
	if got := BroadcastFor(net.ParseIP("203.0.113.7")); got != nil {
		t.Errorf("BroadcastFor(203.0.113.7) = %s, want nil", got)
	}
}

func TestBroadcastForIPv6(t *testing.T) {
	// IPv6 has no broadcast at all, so there is nothing to fall back to.
	if got := BroadcastFor(net.ParseIP("fe80::1")); got != nil {
		t.Errorf("BroadcastFor(fe80::1) = %s, want nil", got)
	}
}

func TestBroadcastForLocalSubnet(t *testing.T) {
	nets := localNets()
	if len(nets) == 0 {
		t.Skip("no non-loopback IPv4 subnet available")
	}

	for _, n := range nets {
		got := BroadcastFor(n.IP)
		if got == nil {
			t.Errorf("BroadcastFor(%s) = nil, want the subnet broadcast", n.IP)
			continue
		}
		if !n.Contains(got) {
			t.Errorf("BroadcastFor(%s) = %s, outside %s", n.IP, got, n)
		}
	}
}

func TestFindRejectsUnusableHints(t *testing.T) {
	// With nothing to recognise the camera by there is nothing to wait for, so
	// this must fail immediately rather than broadcast for the timeout.
	for _, ip := range []net.IP{nil, net.IPv4zero} {
		start := time.Now()
		if _, err := Find(ip, "nonsense", 5*time.Second); err == nil {
			t.Fatalf("Find(%v, \"nonsense\") succeeded, want an error", ip)
		}
		if elapsed := time.Since(start); elapsed > time.Second {
			t.Errorf("took %v to reject unusable hints, want an immediate failure", elapsed)
		}
	}
}

func TestFindSearchesForACloudAddressAlone(t *testing.T) {
	// A camera behind a repeater answers from the address the cloud knows while
	// the neighbour table shows the repeater's hardware address, so an unusable
	// MAC must not stop the search: the address alone still identifies it.
	start := time.Now()
	if _, err := Find(net.ParseIP("203.0.113.7"), "", 300*time.Millisecond); err == nil {
		t.Fatal("expected an error, nothing answers for TEST-NET-3")
	}
	if elapsed := time.Since(start); elapsed < 300*time.Millisecond {
		t.Errorf("gave up after %v, want a search lasting the full timeout", elapsed)
	}
}
