# Appytizer DNS/NRPT handover

## Scope and timeline

This is a real Appytizer defect, but it is **not the original cause** of the older connectivity problem.

- The Appytizer repository's first commit is dated 2026-08-03 23:51 (+03:00).
- Codex connection failures already existed on 2026-07-30 and 2026-08-02.
- Windows logged DNS Client event 1023 on 2026-08-05: the NRPT policy table was corrupt while reading rule `Appytizer` (`0x3F2`).
- On 2026-08-09, `Resolve-DnsName chatgpt.com` failed immediately with the same corruption error even when `-Server 1.1.1.1` was specified.

Therefore Appytizer introduced a second DNS failure on August 3/5, while the older disconnects have a separate LAN-side cause.

## Reproduced defect

`engine/engine.cpp` currently writes this policy:

- Key: `HKLM\SOFTWARE\Policies\Microsoft\Windows NT\DNSClient\DnsPolicyConfig\Appytizer`
- `Name`: `REG_SZ`, value `.local`
- `GenericDNSServers`: `REG_SZ`, value `127.0.0.1`
- `ConfigOptions`: `REG_DWORD`, value `0`

Relevant implementation: `kNrptKey` near line 12 and `Engine::apply_nrpt()` near lines 156-163.

That does not match Microsoft's NRPT Generic DNS Server schema:

- The rule subkey is documented as `{Rule GUID}`.
- `Name` is `REG_MULTI_SZ`, not `REG_SZ`.
- The Generic DNS Server option uses `ConfigOptions = 0x8`, not `0`.
- `GenericDNSServers` is a semicolon-delimited `REG_SZ`.

The implementation also ignores every `RegSetValueExW` result and returns success after only the key-create call succeeds. A partially written or invalid machine policy can therefore be reported as successfully installed.

## Preferred fix

Remove Appytizer's NRPT manipulation entirely and rely on the hosts-file entries it already manages. The README already describes the hosts entry as the browser-resolution path, and the engine creates one entry for every discovered site. This avoids changing machine-wide DNS policy for a per-project development tool.

Also prefer a reserved development suffix such as `.test`; `.local` is reserved for multicast DNS and creates avoidable conflicts with normal LAN discovery.

If NRPT must remain:

1. Generate/use a GUID-form rule key and implement Microsoft's exact value types and flags.
2. Check every registry API result; on any failure, delete the partial rule and return an error.
3. Start and verify the loopback DNS listener before publishing the NRPT rule.
4. Remove the rule on stop, failed startup, uninstall, and extension changes.
5. Do not leave a rule targeting `127.0.0.1` when no DNS listener owns port 53.
6. Add an integration test that installs the rule, confirms `Get-DnsClientNrptRule` can enumerate it, resolves both an Appytizer name and a public name, removes the rule, and confirms public resolution still works.

## Machine remediation already performed

- Backup: `C:\Users\burak\Projects\pc\dns-policy-backup-2026-08-09.reg`
- Removed only: `...\DnsPolicyConfig\Appytizer`
- Cleared the Windows DNS cache.
- Verified the rule is absent and `Get-DnsClientNrptRule` returns without corruption.
- Verified `chatgpt.com` resolves through both `192.168.1.1` and `1.1.1.1`.

Do not run the current Appytizer Engine build before fixing `apply_nrpt()`: startup or an extension change can recreate the corrupt policy.

The separate `EnableMulticast=0` machine policy still exists. Its ownership was not proven, so it was deliberately left unchanged. The Appytizer agent should determine whether Appytizer documentation/setup created it and, if so, add safe uninstall/rollback behavior.

## References

- Microsoft NRPT Generic DNS Server example: https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-gpnrpt/0fb6a915-3dcc-439b-bace-674100c97a25
- Microsoft `Get-DnsClientNrptRule`: https://learn.microsoft.com/en-us/powershell/module/dnsclient/get-dnsclientnrptrule
