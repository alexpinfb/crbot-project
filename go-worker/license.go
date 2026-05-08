package main

import (
	"crypto/ed25519"
	"crypto/x509"
	"encoding/base64"
	"encoding/json"
	"encoding/pem"
	"fmt"
	"os"
)

type LicenseFile struct {
	Payload   string `json:"payload"`
	Signature string `json:"signature"`
}

func verifyLicense(path string) error {
	pubPem, err := os.ReadFile("license_public.pem")
	if err != nil {
		return err
	}

	block, _ := pem.Decode(pubPem)
	if block == nil {
		return fmt.Errorf("invalid public key")
	}

	pubAny, err := x509.ParsePKIXPublicKey(block.Bytes)
	if err != nil {
		return err
	}

	pub := pubAny.(ed25519.PublicKey)

	raw, err := os.ReadFile(path)
	if err != nil {
		return err
	}

	var lic LicenseFile
	if err := json.Unmarshal(raw, &lic); err != nil {
		return err
	}

	sig, err := base64.StdEncoding.DecodeString(lic.Signature)
	if err != nil {
		return err
	}

	ok := ed25519.Verify(pub, []byte(lic.Payload), sig)
	if !ok {
		return fmt.Errorf("license verify failed")
	}

	return nil
}
