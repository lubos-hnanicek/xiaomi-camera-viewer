// Command bridge builds xmbridge.dll, the C-ABI surface over the Xiaomi
// protocol implementation in internal/.
//
// This file is the boundary and nothing else: it marshals between C buffers and
// Go values and keeps the handle table. All behaviour lives in api.go and the
// internal packages, which stay free of cgo so they can be tested with plain
// `go test`.
package main

/*
#cgo CFLAGS: -I${SRCDIR}/../include
#include <stdlib.h>
#include <string.h>
#include "xmbridge_types.h"
*/
import "C"

import (
	"sync"
	"unsafe"

	"github.com/spec8472/xiaomi-viewer/bridge/internal/stream"
)

func main() {}

// --- Handle table -----------------------------------------------------------
//
// cgo forbids handing a Go pointer to C and letting C hold onto it, so sessions
// live in a map here and C gets a one-byte malloc'd block whose address is the
// key. That address is a genuine C pointer, which keeps both the runtime and
// `go vet` happy, and it is guaranteed unique for as long as it is unfreed.

var (
	handlesMu sync.RWMutex
	handles   = map[uintptr]*stream.Session{}
)

func registerSession(s *stream.Session) unsafe.Pointer {
	ptr := C.malloc(1)

	handlesMu.Lock()
	handles[uintptr(ptr)] = s
	handlesMu.Unlock()

	return ptr
}

func lookupSession(handle unsafe.Pointer) *stream.Session {
	if handle == nil {
		return nil
	}
	handlesMu.RLock()
	defer handlesMu.RUnlock()
	return handles[uintptr(handle)]
}

func unregisterSession(handle unsafe.Pointer) *stream.Session {
	if handle == nil {
		return nil
	}

	handlesMu.Lock()
	key := uintptr(handle)
	s := handles[key]
	delete(handles, key)
	handlesMu.Unlock()

	if s != nil {
		C.free(handle)
	}
	return s
}

// --- Buffer helpers ---------------------------------------------------------

func goString(s *C.char) string {
	if s == nil {
		return ""
	}
	return C.GoString(s)
}

func goBytes(s *C.char) []byte {
	if s == nil {
		return nil
	}
	return []byte(C.GoString(s))
}

// writeResponse copies a JSON response into the caller's buffer, following the
// snprintf convention: the required length is always returned, and nothing is
// written when it would not fit.
func writeResponse(payload []byte, out *C.char, capacity C.int) C.int {
	n := len(payload)
	if out == nil || int(capacity) < n {
		return C.int(n)
	}
	C.memcpy(unsafe.Pointer(out), unsafe.Pointer(&payload[0]), C.size_t(n))
	return C.int(n)
}

var versionString = C.CString(Version)

//export xmb_version
func xmb_version() *C.char {
	return versionString
}

//export xmb_call
func xmb_call(method *C.char, request *C.char, out *C.char, capacity C.int) C.int {
	if method == nil {
		return C.XMB_ERR_INVALID_ARG
	}
	return writeResponse(handleCall(goString(method), goBytes(request)), out, capacity)
}

//export xmb_stream_open
func xmb_stream_open(request *C.char, out *C.char, capacity C.int) unsafe.Pointer {
	session, response := openStream(goBytes(request))
	writeResponse(response, out, capacity)

	if session == nil {
		return nil
	}
	return registerSession(session)
}

//export xmb_stream_read
func xmb_stream_read(handle unsafe.Pointer, buf *C.uchar, capacity C.int, meta *C.XmbFrame) C.int {
	session := lookupSession(handle)
	if session == nil {
		return C.XMB_ERR_INVALID_HANDLE
	}

	frame, err := session.Read()
	if err != nil {
		return C.XMB_ERR_EOF
	}

	if meta != nil {
		meta.kind = C.int32_t(frame.Kind)
		meta.codec = C.int32_t(frame.Codec)
		meta.keyframe = 0
		if frame.Keyframe {
			meta.keyframe = 1
		}
		meta.sample_rate = C.int32_t(frame.SampleRate)
		meta.pts_ms = C.int64_t(frame.PTS)
		meta.sequence = C.uint32_t(frame.Sequence)
		meta.size = C.uint32_t(len(frame.Data))
	}

	n := len(frame.Data)
	if buf == nil || int(capacity) < n {
		// The frame is discarded rather than held: a caller whose buffer is too
		// small is better served by growing it and taking the next frame than by
		// stalling the queue behind one oversized access unit.
		return C.XMB_ERR_BUFFER_TOO_SMALL
	}
	if n > 0 {
		C.memcpy(unsafe.Pointer(buf), unsafe.Pointer(&frame.Data[0]), C.size_t(n))
	}

	return C.int(n)
}

//export xmb_stream_command
func xmb_stream_command(handle unsafe.Pointer, request *C.char, out *C.char, capacity C.int) C.int {
	session := lookupSession(handle)
	if session == nil {
		return C.XMB_ERR_INVALID_HANDLE
	}
	return writeResponse(handleStreamCommand(session, goBytes(request)), out, capacity)
}

//export xmb_stream_close
func xmb_stream_close(handle unsafe.Pointer) {
	if session := unregisterSession(handle); session != nil {
		session.Close()
	}
}
