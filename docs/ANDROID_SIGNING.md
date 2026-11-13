# Persistent Android signing

HeavenBench keeps Android compilation and persistent signing separate. The Docker/Gradle pipeline produces the same debug-signed, debuggable development APK without persistent signing credentials. A conditional post-build step signs that APK with the project's retained identity and verifies its public certificate pin. This establishes continuity for future updates; it does not turn the development build into a production build or establish physical-device acceptance.

## Published 1.2.0 identity

The [official HeavenBench 1.2.0 APK](https://github.com/dev-zetta/HeavenBench/releases/tag/v1.2.0) uses application ID `app.zetta.heavenbench`, versionCode **6** and the retained public certificate SHA-256 `1520df3e689ee71ce34a05ebabcbfd2b68c7e672048bbd45879ba789a445b6aa`. It remains debuggable development software. A verified signature establishes package identity, not physical-device installation, rendering or upgrade acceptance.

Published 1.1.1 uses a different certificate; back up imported assets, configuration and reports, then uninstall and reinstall once. A build already signed with the retained key can update in place when the application ID matches and versionCode increases. Historical 1.1.1 release and CI records below retain their original source, APK hashes, certificate and versionCode.

## Trusted CI boundary

The Android workflow retains one APK job. Only a push to `main` or a manual dispatch on `refs/heads/main` in the exact `dev-zetta/HeavenBench` repository may use the `android-signing` GitHub Environment. Its deployment branch policy must allow the `main` branch only. Pull requests, forks and other branches use the empty `android-development` environment, skip persistent signing and retain development artifacts.

Both paths build and validate the development APK first. Persistent signing secrets are supplied only to the conditional signing step; they must never enter Docker build arguments, the build context, BuildKit caches, Gradle configuration or compilation steps. The trusted path uploads the verified signed output as its canonical APK; other events upload the original development APK. Missing official signing credentials, an invalid keystore or a certificate-pin mismatch must fail the trusted path, without falling back to an ephemeral signer. Signing does not create or modify a GitHub release.

Configure these secrets in the protected `android-signing` environment:

| Secret | Value |
| --- | --- |
| `ANDROID_SIGNING_KEYSTORE_BASE64` | Strict Base64 encoding of the retained PKCS12 keystore, using alias `heavenbench`. Base64 is transport encoding, not encryption. |
| `ANDROID_SIGNING_PASSWORD` | The retained keystore and private-key password. The configured PKCS12 identity uses the same password for both. |

The retained post-1.1.1 identity has public certificate SHA-256 `1520df3e689ee71ce34a05ebabcbfd2b68c7e672048bbd45879ba789a445b6aa`. The expected public pin is committed as 64 hexadecimal characters in [android/signing-certificate.sha256](../android/signing-certificate.sha256). Only the public pin belongs in the repository. Never commit a keystore, private key, password or a Base64 copy of the keystore, and never upload them as workflow artifacts.

## Retain and restore the identity

Create or import the signing identity once, privately, and retain an independently recoverable backup outside the checkout and CI caches. Keep the encrypted keystore, its password and the public certificate/fingerprint recoverable through the project's private backup procedure. Record the private backup location separately from public documentation. Access to GitHub environment secrets alone is not a backup plan.

When rebuilding the CI environment or recovering from missing secrets, restore the existing keystore and password and confirm that its public certificate matches the committed pin. Do not generate a replacement key to make a failing signing step pass, change the pin to match an unexpected key, or regenerate the identity on each runner. The APK contains a public certificate, not its private signing key; the published APK cannot recover a lost private key. Without the retained key, the current direct-APK distribution cannot preserve that update identity. Android's [app-signing guide](https://developer.android.com/studio/publish/app-signing) explains the role and retention of the app signing key.

The signing helper [scripts/sign_android_apk.py](../scripts/sign_android_apk.py) handles the retained key only in a private directory under `RUNNER_TEMP`, with directory mode `0700` and keystore mode `0600`. Its signing-tool child environment must omit the Base64 keystore; password arguments reference an environment variable rather than containing the password. Signing-tool output and exception messages must not disclose secret values. Private temporary material must be removed on success and failure, and must stay outside artifact upload paths.

## Verify the signed output

The post-build helper requires an exact certificate-pin match, one signer, and successful APK Signature Scheme v2 and v3 verification. The Android minimum SDK is 26, so this path does not need a JAR/v1 signature or an incremental-install v4 sidecar. Verify ZIP alignment before signing, and verify the final APK without rewriting its payload afterward; the [apksigner documentation](https://developer.android.com/tools/apksigner) describes this ordering.

Re-signing must retain every non-signature ZIP entry and its uncompressed contents, including the manifest, native libraries, resources, assets and ordinary `META-INF` data. The public signing receipt must bind the original and signed APK hashes, certificate pin, signature verification and payload comparison to the workflow source commit, run ID and run attempt. It must contain no private keystore, password or secret environment dump. Preserve the receipt with the APK and its checksum. Local signing controls and package verification are separate from proving that a later fresh CI runner restores the same identity.

## Verified CI checkpoint

This historical signing checkpoint predates the published 1.2.0 APK and keeps its original 1.1.1/versionCode 5 identity.

[Android CI run 41947953242](https://github.com/dev-zetta/HeavenBench/actions/runs/41947953242) passes from source `b0d5836b625af5953e74ff6925d0e30d3aac6030` on a fresh runner with the retained environment secrets. Independent downloaded-artifact verification confirms the committed single certificate pin, v2/v3 signatures, 16 KiB alignment, package/version identity, 158 payload hashes and 1,076 exact source entries. All 158 non-signature entry payloads also match the unchanged published 1.1.1 APK byte-for-byte. The public receipt binds this source, run and attempt to the signed APK SHA-256 `35ba714567ca67f4cac8eb0be161317bf0ee03a6a0bf159b6db78ebdda3f4928`. [Release readiness](RELEASE_READINESS.md#persistent-android-signing-follow-up) records the evidence and its scope; physical-device upgrade acceptance remains unverified.

## Migration from the published development APK

Published HeavenBench 1.1.1 remains bound to source `7930b9eaae5feef8dfae7396e479d534848d721f`, Android versionCode `5` and certificate SHA-256 `e9ada48557382df3482a55a610bac47870dbff5cf78ca367a911c180653d85cf`. Its APK, release assets, version tag and recorded checksums remain unchanged. Its private development key was not retained and cannot be recovered from the APK.

The first APK using the new persistent identity cannot update that published 1.1.1 installation in place. Back up imported assets, configuration and reports before uninstalling 1.1.1 and installing the new signer. Android removes app-specific storage on uninstall; see [Getting started](GETTING_STARTED.md) and the [Android storage instructions](../android/README.md). Future official APKs can update in place when they retain `app.zetta.heavenbench`, the same signing key and an incremented versionCode. Development APKs from pull requests, other branches or unrelated local keys do not inherit this guarantee.

That earlier signing checkpoint kept application version `1.1.1` and versionCode `5` and did not publish a replacement release. The later 1.2.0 identity is recorded above. Production build settings, production-release qualification and physical-device update acceptance remain separate work.
