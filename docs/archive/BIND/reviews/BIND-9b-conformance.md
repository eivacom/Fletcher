# BIND-9b — conformance against the acceptance and the rulings

Graded at `b23f472` by the fresh-context tests-and-acceptance reviewer (T) of the BIND-9b review
(`BIND-9b-codereview.md`), from the tree only; CI results are taken from the records, which the
tree cannot prove. MET, PARTLY, NOT MET; AT MERGE where D-BIND-174 puts the work after BIND-T and
BIND-10, or D-BIND-164 on `main`.

## The acceptance (tracker, BIND-9)

| Bullet | Grade | Evidence or gap |
|---|---|---|
| Part 5: the `ci.*` lanes, `cd.dotnet` with the self-hosted push, the key, Release attach, `verify-tag-version-dotnet`, no dist-tag | MET as built; first run AT MERGE | `cd.dotnet.yml`, `cd.nuget-feed.yml` (`[self-hosted, eiva-network]`), the tag check's `test.sh` (7 cases and the real props) |
| RID matrix `win-x64` + `linux-x64` into one `pack` | MET | `ci.dotnet` two legs, one pack job |
| Licence files, asset isolation | MET | 9a; notice content checked by `check_third_party_notice.sh` on `ci.dotnet`, `ci.gateway`, `ci.protoc` |
| 12 MB packed ceiling (D-BIND-157) | MET | `check_dotnet_packages.sh`; strip with an unchanged-exports check in `ci.dotnet` |
| glibc floor | MET (2.34, D-BIND-169) | `shim_floors.env`, measured = stated; the bullet's text still says 2.38 (T7) |
| MSVC CRT dynamic (D-BIND-155) | MET | `README.package.md`; the loader's `PlatformFloor` |
| NativeAOT on `win-x64` and over `fastdds` | MET | `ci.dotnet` Windows and Linux legs, both frameworks; `AotSmoke/Program.cs`; the shipped README contradicts it (B2) |
| Null in a non-nullable column | MET, with debts | see D-BIND-156; T1, T2 |
| C#-native cross-language rows | MET | `CrossLangTests.cs` (5 facts), the C# native peer, both OS legs; 11/11 on CI |
| Self-hosted runner registered, documented, sole use | STRUCK by D-BIND-176 | no self-hosted runner; the nuget.org setup is runbook step 6 (Q2) |
| `Eiva.Fletcher.Protoc` (D-BIND-71, 158) | MET; "same MAJOR.MINOR" PARTLY | `check_protoc_package.sh`; the Linux consumer runs the Exec; the edition refusal stated; MAJOR.MINOR not asserted (T3); the feed push AT MERGE |
| LGPL relinking decision before the first external publication | PARTLY | D-BIND-140 ruled "for the preview"; whether it covers the release is Q1 |
| `--locked-mode`, lock files committed | MET | `ci.dotnet`; 10 lock files |
| Merge-time: bump, CD publish and dry runs, preview retired, previews cleared | AT MERGE | D-BIND-174 and the runbook; the gate's non-preview path first runs at the retirement (Q5) |

## The rulings

| Ruling | Grade | Evidence or gap |
|---|---|---|
| 153 the release never waits on the runner | MET; SUPERSEDED 2026-10-28 by D-BIND-176 | Release on a GitHub-hosted job; the feed job of its own |
| 154 Ubuntu 24.04 and later | MET as amended by 169 | 154's text not marked amended (T7) |
| 155 dynamic CRT | MET | as above |
| 156 the C++ codec refuses; C# widened | MET | `nanoarrow_codec.cpp`, `arrow-bridge` codec, `GatewayRowCodec.cs`; tests in all three (7, 7, 5 + 3); TypeScript is BIND-T's; gaps T1, T2 |
| 157 ceiling, strip | MET | as above |
| 158 `options.proto` and the Grpc.Tools wiring | MET | the targets; `GrpcConsumer` reads both options back; the opt-out untested (T9) |
| 159 consumer on the packed packages, `ci.pr` and the gate | MET (`ci.pr`) / AT MERGE (gate) | `ci.package-consumer.yml`; `cd.dotnet` |
| 160 slice order | MET | 9b-8 in progress |
| 161 rows in the 8d lane, topic-named lines | MET | `CrossLangTests.cs` |
| 162 the gate builds the gateway and protoc from the tag | MET | `cd.dotnet` |
| 163 the feed gated on `NUGET_EIVA_RUNNER`, dispatchable | MET; SUPERSEDED 2026-10-28 by D-BIND-176 (nuget.org, no runner) | `cd.dotnet`; `cd.nuget-feed.yml` (deleted) |
| 164 a dispatch is a dry run | MET as built / AT MERGE | the release job runs on `push` only |
| 165 the schema reader without data-sharing | MET | `qos_defaults.cpp`; the test pins the factory, not the provider's call |
| 166 vendored texts, NOTICE files, check | MET | the provider recipe refuses without them; the check on three lanes |
| 167 Fast DDS 3.4.3 | MET | five recipes; no 3.4.0 left |
| 168 `lossless` 30 s | MET | `qos_defaults.cpp`; `test_profile_document.cpp` |
| 169 floor 2.34 | MET | `shim_floors.env` |
| 170 pool minimum 32, one trx per framework | MET | both test projects; `LogFilePrefix`; no guard (T4) |
| 171 T12 closed without a test, the README names CMake and the toolchain | MET | `README.package.md` (the CMake number wrong, B3) |
| 172 the profiles' floors, the minimal-machine test, the loader message | MET | `minimal.sh` (cannot pass vacuously), `NativeLoader.cs`, `NativeLoaderTests.cs`; the gateway's 2.38 stated, not guarded (L10, T5) |
| 173 the shim's lockfile; none for the gateway and protoc | MET | `ci.dotnet` both legs, `check_conan_lockfile.sh`, `check_dotnet_packages.sh`, `cd.dotnet`; green at `d52dd42` |

**After the review, 2026-10-28.** The grades the review's findings left short now read: NativeAOT
on `win-x64` MET with the README right (B2); the LGPL row ruled for the release (D-BIND-175's
exception), its wiring AT MERGE after EIVA legal; D-BIND-154's text marked amended by 169 (T7);
the self-hosted rows and 163 superseded by D-BIND-176; `cd.protoc` gated as `cd.dotnet` is
(D-BIND-177), AT MERGE; the gate's non-preview path AT MERGE, first run at the retirement
(D-BIND-179). "Same MAJOR.MINOR" stays PARTLY (T3, carried as known). **On CI:** `9c9e2bc`,
`ci.pr` 40702620300 green 61/61 on the first attempt, every side workflow green; BIND-9b closed.
