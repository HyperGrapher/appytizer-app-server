# Appytizer implementation requirements

This document reflects the implemented product contract. Historical `.local`,
custom-suffix, and NRPT designs are obsolete and intentionally unsupported.

## Site model

- A direct child of the configured projects root maps to `<folder>.test`.
- `.test` is fixed and is not a setting.
- Folder names must be single DNS labels. Invalid and case-colliding folders
  remain visible with an error but are not published.
- The Windows hosts file remains the browser hostname-resolution mechanism.

## Trusted HTTPS

- HTTPS is enabled by default and can be disabled through Engine-owned config.
- Appytizer owns one RSA-3072, SHA-256, ten-year root CA in
  `%ProgramData%\Appytizer\certificates` and trusts its exact certificate in
  `LocalMachine\Root`.
- Each valid site receives an exact RSA-2048 certificate with matching CN/SAN,
  Server Authentication EKU, SHA-256 signature, 397-day validity, and renewal
  within 30 days of expiry.
- Ownership metadata records both SHA-1 and SHA-256 fingerprints. Cleanup never
  selects certificates by subject name.
- CA and leaf private keys grant access only to SYSTEM and Administrators. The
  Engine service and its nginx child run as SYSTEM.

## nginx activation

- HTTPS mode redirects known port-80 hosts with `308`, serves content on 443,
  returns `404` for unknown HTTP hosts, and rejects unknown TLS handshakes.
- HTTP mode serves known sites on port 80 and emits no port-443 listener.
- No HSTS header is emitted.
- A complete staging tree must pass `nginx -t` before replacing the active tree.
  Certificate or validation failures retain the last active configuration.
- Leaf certificates for deleted sites are removed only after successful
  activation.

## Ownership boundaries

- The Engine is the only configuration authority. The UI reads and writes
  settings over IPC.
- `config.get` and `config.set` expose `https_enabled`.
- `tls.status` exposes `ready`, `trusted`, `site_certificate_count`,
  `earliest_expiry`, and `error`.
- TLS repair launches `AppytizerEngine --provision-tls` through UAC, refreshes
  status, and requests site/nginx regeneration.

## Installation lifecycle

- Elevated setup provisions TLS before service installation and aborts on
  failure.
- Upgrades preserve valid certificate material and restart an existing stopped
  service.
- Uninstall stops the service, removes the exact recorded trust entry and owned
  certificate tree, and then deletes the service.
