// Package lan finds a camera on the local network when the cloud will not say
// where it is.
//
// The Mi cloud caches the address a device last reported and sometimes has
// nothing at all, handing back 0.0.0.0. The device list still carries a MAC
// though, and cameras answer a broadcast CS2 discovery packet, so the two
// together locate the camera without the user hunting through their router.
package lan

import (
	"encoding/binary"
	"fmt"
	"net"
	"strings"
	"sync"
	"syscall"
	"time"
	"unsafe"
)

// cs2Port is where the CS2 discovery listener lives on every supported camera.
const cs2Port = 32108

// NormalizeMAC strips the separators the cloud and the OS disagree about, so
// "18:50:73:12:6F:8D" and "18-50-73-12-6f-8d" compare equal.
func NormalizeMAC(mac string) string {
	var b strings.Builder
	for _, r := range strings.ToLower(mac) {
		if (r >= '0' && r <= '9') || (r >= 'a' && r <= 'f') {
			b.WriteRune(r)
		}
	}
	return b.String()
}

// BroadcastFor returns the directed broadcast address of the locally attached
// subnet containing ip, or nil if the host is not on one of our subnets.
//
// The limited broadcast 255.255.255.255 would be simpler but needs SO_BROADCAST
// and would leak the probe onto every interface, including virtual adapters.
func BroadcastFor(ip net.IP) net.IP {
	ip = ip.To4()
	if ip == nil {
		return nil
	}

	for _, n := range localNets() {
		if n.Contains(ip) {
			return broadcastOf(n)
		}
	}
	return nil
}

func broadcastOf(n *net.IPNet) net.IP {
	local := n.IP.To4()
	mask := net.IP(n.Mask).To4()
	if local == nil || mask == nil {
		return nil
	}

	b := make(net.IP, net.IPv4len)
	for i := range b {
		b[i] = local[i] | ^mask[i]
	}
	return b
}

// localNets returns every directly attached IPv4 network, skipping loopback and
// single-host masks that cannot carry a broadcast.
func localNets() []*net.IPNet {
	addrs, err := net.InterfaceAddrs()
	if err != nil {
		return nil
	}

	var nets []*net.IPNet
	for _, a := range addrs {
		n, ok := a.(*net.IPNet)
		if !ok || n.IP.To4() == nil || n.IP.IsLoopback() {
			continue
		}
		if ones, bits := n.Mask.Size(); bits != 32 || ones >= 31 {
			continue
		}
		nets = append(nets, n)
	}
	return nets
}

// Find broadcasts a CS2 discovery packet on every attached subnet and returns
// the address of the camera identified by either hint it is given.
//
// A responder is the camera if it answers from cloudIP, the address the cloud
// last recorded for it, or failing that if its hardware address matches mac.
// Both are needed. The cloud's address goes stale when a camera moves, which is
// what the MAC is for; and the MAC only matches when the camera's own frames
// reach us, which is not so behind a repeater or mesh node that answers ARP on
// the camera's behalf with an address of its own.
//
// Either hint may be absent. Nothing is dialled that did not just answer, so a
// cloudIP that no longer belongs to the camera costs nothing.
func Find(cloudIP net.IP, mac string, timeout time.Duration) (net.IP, error) {
	want := NormalizeMAC(mac)
	if len(want) != 12 {
		want = ""
	}
	if cloudIP = cloudIP.To4(); cloudIP != nil && cloudIP.IsUnspecified() {
		cloudIP = nil
	}
	if want == "" && cloudIP == nil {
		return nil, fmt.Errorf(
			"lan: nothing to look for: no address, and %q is not a usable MAC", mac)
	}

	nets := localNets()
	if len(nets) == 0 {
		return nil, fmt.Errorf("lan: no local IPv4 network to search")
	}

	conn, err := net.ListenUDP("udp4", nil)
	if err != nil {
		return nil, fmt.Errorf("lan: %w", err)
	}
	defer conn.Close()

	deadline := time.Now().Add(timeout)
	_ = conn.SetDeadline(deadline)

	var stop sync.Once
	done := make(chan struct{})
	defer stop.Do(func() { close(done) })

	go func() {
		req := []byte{0xF1, 0x30, 0x00, 0x00} // magic, LAN search
		for {
			for _, n := range nets {
				if b := broadcastOf(n); b != nil {
					_, _ = conn.WriteToUDP(req, &net.UDPAddr{IP: b, Port: cs2Port})
				}
			}
			select {
			case <-done:
				return
			case <-time.After(time.Second):
			}
			if time.Now().After(deadline) {
				return
			}
		}
	}()

	// Each address is checked once: cameras retransmit, and an ARP lookup per
	// duplicate would slow the search down for no benefit. The addresses are
	// kept for the error, which is otherwise silent about whether the subnet
	// was empty or full of devices that simply did not match.
	var checked []string
	seen := map[string]bool{}
	buf := make([]byte, 1200)

	for {
		n, addr, err := conn.ReadFromUDP(buf)
		if err != nil {
			return nil, notFound(checked, err)
		}
		if n < 4 || buf[0] != 0xF1 {
			continue
		}

		if cloudIP != nil && addr.IP.Equal(cloudIP) {
			return append(net.IP(nil), addr.IP...), nil
		}

		key := addr.IP.String()
		if seen[key] {
			continue
		}
		seen[key] = true
		checked = append(checked, key)

		if want == "" {
			continue
		}
		got, err := hardwareAddr(addr.IP)
		if err != nil {
			continue // gone from the neighbour table already
		}
		if NormalizeMAC(got.String()) == want {
			return append(net.IP(nil), addr.IP...), nil
		}
	}
}

// notFound distinguishes a subnet where nothing answered at all from one where
// devices answered and none of them was the camera. The first is a camera that
// is switched off, or replies being dropped before they arrive; the second is a
// camera whose address and hardware address are both no longer what the cloud
// believes, and knowing which devices did answer is where that search starts.
func notFound(checked []string, err error) error {
	if len(checked) == 0 {
		return fmt.Errorf("lan: nothing on the local network answered: %w", err)
	}
	return fmt.Errorf(
		"lan: none of the %d device(s) that answered was this camera (%s): %w",
		len(checked), strings.Join(checked, ", "), err)
}

var (
	iphlpapi    = syscall.NewLazyDLL("iphlpapi.dll")
	procSendARP = iphlpapi.NewProc("SendARP")
)

// hardwareAddr resolves the MAC of a host on a directly attached subnet. The
// device has just sent us a datagram, so the entry is in the neighbour table
// and this does not put a packet on the wire.
func hardwareAddr(ip net.IP) (net.HardwareAddr, error) {
	ip4 := ip.To4()
	if ip4 == nil {
		return nil, fmt.Errorf("lan: %s is not IPv4", ip)
	}

	// SendARP takes the address as a ULONG already in network byte order, which
	// is what reading the octets little-endian on a little-endian host gives.
	dest := binary.LittleEndian.Uint32(ip4)

	var mac [8]byte
	length := uint32(len(mac))

	r, _, _ := procSendARP.Call(
		uintptr(dest), 0,
		uintptr(unsafe.Pointer(&mac[0])),
		uintptr(unsafe.Pointer(&length)))

	if r != 0 {
		return nil, fmt.Errorf("lan: SendARP for %s failed with code %d", ip, r)
	}
	if length == 0 || length > uint32(len(mac)) {
		return nil, fmt.Errorf("lan: SendARP for %s returned %d bytes", ip, length)
	}

	return net.HardwareAddr(mac[:length]), nil
}
