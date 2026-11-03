# Security policy

## Reporting a vulnerability

Please report security problems privately, through GitHub's
[private vulnerability reporting](https://github.com/gregtakacs/gorb-os/security/advisories/new),
not in a public issue. Say what you found, how to reproduce it, and what it lets an
attacker do. You will get an answer within a week.

## What is in scope

- the firmware (`main/`): its HTTP API, provisioning, OTA, the app sandbox and grants;
- Orb Manager (`tools/orb-manager`): its local server, the control token, the device proxy,
  and the files it installs;
- the `.orbapp` and `.orbtheme` readers, on the orb and in the hub.

The orb's LAN API is plain HTTP with no authentication by design: anyone on the same
network can manage an orb. Reports that only restate that are known. Reports that get past
it (from off the LAN, from an app past its grants, or into the computer running Orb
Manager) are what we want to hear about.

## Supported versions

Fixes land on `main` and ship in the next release. Only the latest release is supported.
