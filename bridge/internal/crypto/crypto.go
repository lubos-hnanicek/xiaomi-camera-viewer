// Package crypto implements the key agreement and stream cipher used by the
// Xiaomi MISS protocol.
//
// Derived from go2rtc pkg/xiaomi/crypto (MIT). See bridge/NOTICE.md.
//
// The camera and client each generate a Curve25519 keypair; the client's public
// key goes to the Xiaomi cloud, which returns the camera's public key along with
// a signature proving it. Both sides then derive the same shared secret, and
// every command and media payload afterwards is ChaCha20 keystream-XORed with
// it under an 8-byte per-message nonce.
package crypto

import (
	"crypto/rand"
	"encoding/hex"

	"golang.org/x/crypto/chacha20"
	"golang.org/x/crypto/nacl/box"
)

// GenerateKey returns a fresh ephemeral (public, private) Curve25519 keypair.
func GenerateKey() ([]byte, []byte, error) {
	public, private, err := box.GenerateKey(rand.Reader)
	if err != nil {
		return nil, nil, err
	}
	return public[:], private[:], err
}

// CalcSharedKey derives the session key from the camera's public key and our
// private key. Both arguments are hex-encoded.
func CalcSharedKey(devicePublicHex, clientPrivateHex string) ([]byte, error) {
	var sharedKey, publicKey, privateKey [32]byte
	if _, err := hex.Decode(publicKey[:], []byte(devicePublicHex)); err != nil {
		return nil, err
	}
	if _, err := hex.Decode(privateKey[:], []byte(clientPrivateHex)); err != nil {
		return nil, err
	}
	box.Precompute(&sharedKey, &publicKey, &privateKey)
	return sharedKey[:], nil
}

// Encode prefixes a random 8-byte nonce and XORs src with the ChaCha20
// keystream derived from it.
func Encode(src, key32 []byte) ([]byte, error) {
	dst := make([]byte, len(src)+8)

	if _, err := rand.Read(dst[:8]); err != nil {
		return nil, err
	}

	nonce12 := make([]byte, 12)
	copy(nonce12[4:], dst[:8])

	c, err := chacha20.NewUnauthenticatedCipher(key32, nonce12)
	if err != nil {
		return nil, err
	}

	c.XORKeyStream(dst[8:], src)

	return dst, nil
}

// Decode reverses Encode: the first 8 bytes of src are the nonce.
func Decode(src, key32 []byte) ([]byte, error) {
	if len(src) < 8 {
		return nil, errShort
	}
	return DecodeNonce(src[8:], src[:8], key32)
}

// DecodeNonce decrypts src with an externally supplied 8-byte nonce.
func DecodeNonce(src, nonce8, key32 []byte) ([]byte, error) {
	nonce12 := make([]byte, 12)
	copy(nonce12[4:], nonce8)

	c, err := chacha20.NewUnauthenticatedCipher(key32, nonce12)
	if err != nil {
		return nil, err
	}

	dst := make([]byte, len(src))
	c.XORKeyStream(dst, src)

	return dst, nil
}

type cryptoError string

func (e cryptoError) Error() string { return string(e) }

const errShort = cryptoError("xiaomi: encrypted payload shorter than its nonce")
