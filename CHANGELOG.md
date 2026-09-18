## AgentNet proto v2 (2026-09-18)

- `carrier_identity_sign` / `carrier_identity_verify` (XEdDSA, JS-compatible), `carrier_export_secret_key`
- userinfo: `avatar_url`, `url`, `ens`, `extra`; `carrier_set/get_self_profile_ext`, `carrier_get_friend_profile_ext`
- client metadata now sent and surfaced: `carrier_set/get_client_info`, `carrier_get_friend_client_info`; `CARRIER_AGENTNET_PROTO_VERSION` = 2
- unconfirmed messages fall back to Express after 30 s (`do_unconfirmed_expire`); Express HTTP timeout 15 s
- bulk messages over 5 MB refused toward legacy (proto 0) peers
- `tests/unit/identity_test` (`-DENABLE_UNIT_TESTS=ON`), vectors from curve25519-js

03/02/2019 Tang Zhilong stiartsly@gmail.com

**version 5.2.2**, main changes to previous version:

```markdown
- Update checksum SHA256 of depedency libcrystal and it's download address;
- Update License to be GPLv3 with regards to c-toxcore project.
```

01/25/2019 Tang Zhilong <stiartsly@gmail.com>
**Version 5.2.1**, main changes to previous version: 

	- Refactored origin scripts-based build system to CMake-based build system;
	- Support Carrier SDK implementation on Windows platform (x86/x64);
	- Support carrier group without central administration, and group peers should be required connected 		to carrier network;
	- Support file transfer between two peers with pull-driven and resume from break-point enabled.
	- Support for session to have it's own cookie or bundle data;
	- Enlarge the data capacity when using API of sending invitation request/reply with data;
	- Refactored error codes and added APIs to get error description from error number;
	- Add testsuite to verify feature of group and file transfer;
	- Add app demo "elafile" demonstrate how we use APIs of file transfers;
	- Add command tool "elaerr" to check what error description to specific carrier errno number;
	- Upgrade underlying dependency project - toxcore.
	- Optimizations and bugfixes to origin carrier/session/stream;
	- Support CI for all platforms (Linux/Macos/Windows/Android/iOS)

08/14/2018 Tang Zhilong <stiartsly@gmail.com>
**Version 5.1**, main changes listed:

	- Carrier: peer to peer message framework.
	- Session: peer to peer session framework (Oriented to stream of transferring data)

