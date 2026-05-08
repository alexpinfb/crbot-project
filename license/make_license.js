const crypto = require("crypto");
const fs = require("fs");

const privateKey = fs.readFileSync("license_private.pem");
const user = process.argv[2];

if (!user) {
  console.log("Usage: node license/make_license.js USERNAME");
  process.exit(1);
}

const payload = JSON.stringify({
  user,
  createdAt: Date.now()
});

const signature = crypto.sign(null, Buffer.from(payload), privateKey).toString("base64");

const license = Buffer.from(JSON.stringify({
  payload,
  signature
})).toString("base64url");

console.log("LICENSE_KEY=" + license);
