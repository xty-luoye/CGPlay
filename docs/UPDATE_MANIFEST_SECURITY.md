# Update Manifest Security

## Scope

CGPlay already verifies component archive SHA-256 values when a manifest provides a non-empty sha256. The first productization batch adds release-time validation without changing the current runtime update behavior or introducing a private key.

## Release Contract

1. Every downloadable component archive has a lowercase 64-character SHA-256 value in resources/component_manifest.json.
2. The exact UTF-8 manifest bytes may be signed with an offline RSA key using SHA-256.
3. Only the public key is distributed. Private keys stay outside the repository and build machines.
4. The detached binary signature uses the suffix .sig and covers the manifest bytes without canonicalization or reformatting.
5. Key rotation uses a release-side key identifier and an overlap period where the updater trusts the old and new public keys.
6. Manifest and component URLs use HTTPS; archive names, install subdirectories, and required files must be safe relative paths without traversal.
7. A release job that claims signed-manifest support must use `-RequireSignature`; an omitted signature/key pair then fails validation instead of being reported as optional.

## Validation Tool

Development schema/hash check:

~~~powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools/validate_component_manifest.ps1
~~~

Release check with component archives:

~~~powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools/validate_component_manifest.ps1 -PayloadRoot C:\release\components -RequireHashes -RequirePayloads
~~~

Optional detached signature verification requires openssl and a public key:

~~~powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools/validate_component_manifest.ps1 -SignaturePath C:\release\component_manifest.json.sig -PublicKeyPath C:\release\cgplay-update-public.pem
~~~

Release-required detached signature verification:

~~~powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools/validate_component_manifest.ps1 -RequireSignature -SignaturePath C:\release\component_manifest.json.sig -PublicKeyPath C:\release\cgplay-update-public.pem
~~~

No private key, private-key path, or signing secret belongs in the repository, manifest, diagnostics bundle, or installer staging tree.

## Safe Rollout

- Phase 1, completed: release-time schema/hash/signature validation framework.
- Phase 2: publish real archive hashes and detached signatures from a protected release job.
- Phase 3: add runtime signature enforcement behind a compatibility switch, with cached-manifest rollback protection.
- Phase 4: require signed manifests after existing installations have received the trusted public key.

Runtime enforcement is intentionally deferred because changing manifest acceptance can strand existing installations if server artifacts or keys are not ready.
