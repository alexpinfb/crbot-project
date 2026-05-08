const crypto = require("crypto");
const fs = require("fs");

const { publicKey, privateKey } = crypto.generateKeyPairSync("ed25519");

fs.writeFileSync("license_private.pem", privateKey.export({
  type: "pkcs8",
  format: "pem"
}));

fs.writeFileSync("license_public.pem", publicKey.export({
  type: "spki",
  format: "pem"
}));

console.log("Generated license_private.pem and license_public.pem");
