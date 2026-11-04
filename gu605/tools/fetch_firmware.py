#!/usr/bin/env python3
"""fetch_firmware.py --lock gu605/firmware.lock --out DIR [--ref REF]

Downloads the i915 firmware files listed in the lock file from GitLab's linux-firmware at a fixed ref into DIR (flat,
base names, as tools/link.sh expects), checks each SHA-256 against the lock, and writes DIR/FIRMWARE.txt (ref, commit,
sizes, hashes, licence). Downloads are untrusted data: nothing is executed, sizes are bounded, names are fixed."""
import argparse, hashlib, json, os, sys, urllib.parse, urllib.request

API = "https://gitlab.com/api/v4/projects/kernel-firmware%2Flinux-firmware"
RAW = "https://gitlab.com/kernel-firmware/linux-firmware/-/raw/{ref}/{path}"
MAX = 8 << 20


def get(url, limit=MAX):
    req = urllib.request.Request(url, headers={"User-Agent": "gu605-ci"})
    with urllib.request.urlopen(req, timeout=60) as r:
        data = r.read(limit + 1)
    if len(data) > limit:
        sys.exit(f"STOP: {url} is larger than {limit} bytes")
    return data


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--lock", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--ref")
    a = ap.parse_args()
    ref, files = None, []
    for line in open(a.lock):
        line = line.split("#", 1)[0].strip()
        if not line:
            continue
        k, v = line.split()
        if k == "ref":
            ref = v
        else:
            files.append((k, v))
    ref = a.ref or ref
    if ref == "latest":
        tags = json.loads(get(API + "/repository/tags?per_page=1&order_by=updated"))
        ref = tags[0]["name"]
        print(f"::warning::firmware ref 'latest' resolved to tag {ref} ({tags[0]['commit']['id']}): pin it in gu605/firmware.lock")
    commit = json.loads(get(API + "/repository/commits/" + urllib.parse.quote(ref, safe="")))["id"]
    os.makedirs(a.out, exist_ok=True)
    report = [f"linux-firmware ref {ref} commit {commit}", "source https://gitlab.com/kernel-firmware/linux-firmware",
              "licence LICENSE.i915 (Intel: redistribution in binary form permitted, see the file)"]
    bad = 0
    for path, want in files:
        data = get(RAW.format(ref=urllib.parse.quote(ref, safe=""), path=path))
        h = hashlib.sha256(data).hexdigest()
        name = os.path.basename(path)
        with open(os.path.join(a.out, name), "wb") as f:
            f.write(data)
        state = "ok" if h == want else ("UNPINNED" if want == "-" else "MISMATCH")
        if state == "MISMATCH":
            bad += 1
        if state == "UNPINNED":
            print(f"::warning::{path} is not pinned: sha256 {h}")
        print(f"{state:8s} {path} {len(data)} B sha256 {h}")
        report.append(f"{path} {len(data)} {h} {state}")
    if "i915/mtl_guc_70.bin" in [p for p, _ in files]:
        g = open(os.path.join(a.out, "mtl_guc_70.bin"), "rb").read()
        # CSS header (struct uc_css_header): sw version at 0x58? report the dword the i915 log names, for the record
        report.append(f"mtl_guc_70.bin header dwords 0x58..0x5f: {g[0x58:0x60].hex()}")
    open(os.path.join(a.out, "FIRMWARE.txt"), "w").write("\n".join(report) + "\n")
    if bad:
        sys.exit(f"STOP: {bad} firmware file(s) do not match gu605/firmware.lock")


if __name__ == "__main__":
    main()
