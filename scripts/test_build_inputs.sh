#!/usr/bin/env bash
# The Apple build driver's input verification (Sources/BuildScripts/
# XCFrameworkBuild): BuildInputs refuses a clone that is not at the commit
# versions.json records and an archive whose SHA-256 differs, and refuses a
# pin that records neither; ComponentPin folds overrides.apple in. Compiled
# VERBATIM from base.swift and main.swift with the driver's own Utility, run
# against throwaway git repositories and files, then against the real
# versions.json: every git source the driver clones has a commit and every
# prebuilt it downloads has a sha256. Downloads and builds nothing.
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
E="$root/scripts/extract.py"
base="$root/Sources/BuildScripts/XCFrameworkBuild/base.swift"
main="$root/Sources/BuildScripts/XCFrameworkBuild/main.swift"

cat > "$work/main.swift" <<'SWIFT'
import CryptoKit
import Foundation
enum BaseBuild { static let defaultPath = "/usr/bin:/bin" }
SWIFT
python3 "$E" range "$base" "$work/utility.swift" \
    --first 'enum Utility {' --last '    /// Every regular file in `directory`, recursively.'
cat "$work/utility.swift" >> "$work/main.swift"
echo '}' >> "$work/main.swift"
python3 "$E" range "$base" "$work/inputs.swift" \
    --first 'struct BuildInputError: LocalizedError {' --last 'class BaseBuild {'
cat "$work/inputs.swift" >> "$work/main.swift"
python3 "$E" range "$main" "$work/pin.swift" \
    --first 'struct ComponentPin: Decodable {' --last '/// Reader for the repository-root'
cat "$work/pin.swift" >> "$work/main.swift"
cat >> "$work/main.swift" <<'SWIFT'
// A stand-in for the driver's Library: the fields BuildInputs reads.
struct Library {
    let canonicalName: String
    let url: String
    let version: String
    let commit: String?
    let sha256: String?
}

func expectThrow(_ what: String, _ body: () throws -> Void) {
    do {
        try body()
        preconditionFailure("\(what): did not throw")
    } catch {
        precondition(error is BuildInputError, "\(what): threw \(error), not BuildInputError")
    }
}

let work = URL(fileURLWithPath: CommandLine.arguments[1])
let repo = work.appendingPathComponent("repo")
try FileManager.default.createDirectory(at: repo, withIntermediateDirectories: true)
func git(_ args: [String]) throws -> String {
    try Utility.launch(path: "/usr/bin/git", arguments: ["-c", "user.name=t", "-c", "user.email=t@t"] + args,
                       isOutput: true, currentDirectoryURL: repo)
}
_ = try git(["init", "-q"])
try "one".write(to: repo.appendingPathComponent("f"), atomically: true, encoding: .utf8)
_ = try git(["add", "f"]); _ = try git(["commit", "-qm", "one"])
let first = try BuildInputs.headCommit(of: repo)
try "two".write(to: repo.appendingPathComponent("f"), atomically: true, encoding: .utf8)
_ = try git(["commit", "-qam", "two"])
let second = try BuildInputs.headCommit(of: repo)
precondition(first.count == 40 && first != second)

func lib(commit: String? = nil, sha256: String? = nil) -> Library {
    Library(canonicalName: "x", url: "https://example.invalid/x", version: "v1", commit: commit, sha256: sha256)
}
// A clone at the pin passes, in either case; any other commit, or no pin, fails.
try BuildInputs.verifyCommit(of: repo, library: lib(commit: second))
try BuildInputs.verifyCommit(of: repo, library: lib(commit: second.uppercased()))
expectThrow("a clone at another commit") { try BuildInputs.verifyCommit(of: repo, library: lib(commit: first)) }
expectThrow("a git pin without a commit") { try BuildInputs.verifyCommit(of: repo, library: lib()) }
expectThrow("an empty commit") { try BuildInputs.verifyCommit(of: repo, library: lib(commit: "")) }

// SHA-256 of a file, against a known vector and a file over one read chunk.
let abc = work.appendingPathComponent("abc")
try "abc".write(to: abc, atomically: true, encoding: .utf8)
let abcHash = try BuildInputs.sha256(of: abc)
precondition(abcHash == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad")
let big = work.appendingPathComponent("big")
try Data(repeating: 0x61, count: 3 << 20 | 7).write(to: big)
let shasum = try Utility.launch(path: "/usr/bin/shasum", arguments: ["-a", "256", big.path], isOutput: true)
let bigHash = try BuildInputs.sha256(of: big)
precondition(bigHash == String(shasum.prefix(64)), "multi-chunk hash differs from shasum")
expectThrow("a prebuilt pin without a sha256") { _ = try BuildInputs.expectedSHA256(for: lib()) }
let lowered = try BuildInputs.expectedSHA256(for: lib(sha256: "AB"))
precondition(lowered == "ab")

// overrides.apple folds in; other groups' overrides do not.
let sample = """
{"kind": "prebuilt", "version": "1", "url": "https://a/x.zip", "sha256": "base",
 "overrides": {"android": {"kind": "git", "url": "https://b", "commit": "c"},
               "apple": {"sha256": "apple"}}}
""".data(using: .utf8)!
let pin = try JSONDecoder().decode(ComponentPin.self, from: sample)
precondition(pin.sha256 == "apple" && pin.kind == "prebuilt" && pin.url == "https://a/x.zip" && pin.commit == nil)

// The real versions.json, read the way the driver reads it: every component
// the apple driver acquires has what its verification needs.
struct Root: Decodable { let components: [String: ComponentPin] }
let versions = try JSONDecoder().decode(Root.self, from: Data(contentsOf: URL(fileURLWithPath: CommandLine.arguments[2])))
let driverComponents = CommandLine.arguments[3].split(separator: ",").map(String.init)
var gitCount = 0, zipCount = 0
for name in driverComponents {
    guard let p = versions.components[name] else { preconditionFailure("versions.json has no \(name)") }
    switch p.kind {
    case "git":
        precondition((p.commit ?? "").count == 40, "\(name): git pin without a full commit")
        gitCount += 1
    case "prebuilt":
        precondition((p.sha256 ?? "").count == 64, "\(name): prebuilt pin without a sha256")
        zipCount += 1
    default:
        preconditionFailure("\(name): unexpected kind \(p.kind)")
    }
}
print("build input verification passed (\(gitCount) git pins with commits, \(zipCount) prebuilt archives with sha256)")
SWIFT
# The libraries the driver's `builds:` list acquires, by versions.json name.
components="$(cd "$root" && python3 - <<'PY'
import re, sys
sys.path.insert(0, "scripts")
import patches
names = patches.apple_driver_components()
print(",".join(sorted(names)))
PY
)"
xcrun swiftc -swift-version 5 "$work/main.swift" -o "$work/test"
"$work/test" "$work" "$root/versions.json" "$components" > "$work/test.log" 2>&1 || { cat "$work/test.log"; exit 1; }
tail -n 1 "$work/test.log"
