# Security

Kenning is part of how you sign in, so a security problem in it matters. Thank you for reporting one.

## Reporting a vulnerability

Please **don't open a public issue**. Report it privately instead, through [GitHub's private vulnerability reporting](https://github.com/RishonDev/kenning/security/advisories/new).

Include what you can of:

- what an attacker could do, and what they need first, such as physical access, a local account or a photo of the user
- the steps to reproduce it
- your distribution, Plasma version, Kenning version (`kenning version`) and camera (IR or ordinary webcam)

You'll get a reply within a week. Once a fix is released, the advisory is published with credit to you, unless you'd rather not be named.

## Supported versions

Only the latest release gets security fixes.

## What counts

Examples of what to report:

- signing in, unlocking or approving as someone without their face or password
- reading or replacing someone's face models
- a way to make Kenning's service or its root helper run something for another user
- getting around the rate limit on failed attempts, for faces or fingerprints
- getting, guessing or reusing the token Kenning's polkit dialog uses for a finger or face, or approving an administrator prompt with it without that finger or face
- making the lock screen or the login screen read a finger the user didn't choose to use

These are known limits, which the README describes, rather than vulnerabilities:

- an ordinary webcam being fooled by a photo or video, since Kenning has no liveness check
- someone who looks very much like you being recognised
