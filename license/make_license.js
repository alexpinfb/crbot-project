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

const sign = crypto.sign(null, Buffer.from(payload), privateKey);

const license = {
  payload,
  signature: sign.toString("base64")
};

const out = `${user}.license.txt`;
fs.writeFileSync(out, JSON.stringify(license, null, 2));

console.log(`Generated ${out}`);
