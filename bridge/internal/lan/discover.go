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

// FindByMAC broadcasts a CS2 discovery packet on every attached subnet and
// returns the address of the device whose hardware address matches mac.
func FindByMAC(mac string, timeout time.Duration) (net.IP, error) {
	want := NormalizeMAC(mac)
	if len(want) != 12 {
		return nil, fmt.Errorf("lan: %q is not a usable MAC address", mac)
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
	// duplicate would slow the search down for no benefit.
	checked := map[string]bool{}
	buf := make([]byte, 1200)

	for {
		n, addr, err := conn.ReadFromUDP(buf)
		if err != nil {
			return nil, fmt.Errorf("lan: no device with MAC %s answered: %w", mac, err)
		}
		if n < 4 || buf[0] != 0xF1 {
			continue
		}

		key := addr.IP.String()
		if checked[key] {
			continue
		}
		checked[key] = true

		got, err := hardwareAddr(addr.IP)
		if err != nil {
			continue // gone from the neighbour table already
		}
		if NormalizeMAC(got.String()) == want {
			ip := make(net.IP, len(addr.IP))
			copy(ip, addr.IP)
			return ip, nil
		}
	}
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
