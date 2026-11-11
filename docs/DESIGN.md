# How Kenning works, and what it trusts

This is the design written down for review. If you can find a way around it, please
say so: in the design review discussion, or privately as [SECURITY.md](../SECURITY.md)
explains.

## Goal

Signing in by looking at the computer, as a convenience next to the password, on the
lock screen, the login screen, `sudo` and administrator prompts.

**Kenning tries to stop:**

- someone who has your laptop but not your password, including after it was stolen
  while switched off;
- a housemate or colleague signing in as you while you're away;
- someone using a printed photo or a screen, when the camera is an infrared (IR) one;
- someone connected over SSH getting a face scan approved by whoever sits at the
  computer;
- other local users and programs reading your face data.

**Kenning doesn't try to stop (non-goals):**

- a photo or video held up to an ordinary webcam: there is no liveness check;
- identical twins and close look-alikes, beyond the match threshold;
- masks, 3D models, or attacks on the camera hardware or its driver;
- anyone with root, or with your unlocked session;
- anyone with the disk who can read `/etc/kenning/models` (use disk encryption).

Face unlock never replaces the password: the password works everywhere, always, and it
alone unlocks KWallet and disk encryption.

## The parts

```
 sudo, login screen, lock screen, polkit        System Settings › Face & Fingerprint
          │ PAM                                          │ D-Bus (KAuth)
          ▼                                              ▼
   pam_kenning.so  ──  /run/kenning.sock  ──►  kenning daemon      settings helper (root)
   (in the caller's      (Unix socket,         (user "kenning",    edits config, PAM files,
    process)              peer credentials)     camera + models)   themes; asks for Set Up
                               ▲                     │ system bus
   Kenning's polkit dialog ────┘                     ▼
   (the user, finger or face → token)              fprintd (fingerprint reader)
```

| Part | Runs as | Does |
|---|---|---|
| `pam_kenning.so` | whatever loads it (root for login, sudo; the user for the lock screen) | decides whether a scan is allowed here, asks the service, maps its answer to a PAM result |
| `kenning daemon` | the `kenning` system user, socket-activated | owns the camera and the face models, runs detection, recognition and Set Up's capture, asks fprintd for fingers, keeps the rate limits and the polkit tokens |
| settings helper | root, through KAuth/polkit | writes `/etc/kenning/config.ini`, adds or removes `pam_kenning.so` in `/etc/pam.d`, asks the service for Set Up, installs themes |
| Kenning's polkit dialog | the user, under their service manager | Plasma's polkit-kde-agent-1 with Fingerprint and Face beside the password; passes a token from the service to `pam_kenning.so grant` |
| System Settings page | the user | the interface; never touches face data itself |
| lock screen add-on, login manager patch | Plasma's own processes | show the scan; they read PAM messages, they don't decide anything |

**Why a service:** OpenCV (image decoding, neural networks) never loads into programs
that authenticate, such as `sudo` or the login manager, and the face models can belong
to one unprivileged user instead of being readable by everyone. It follows fprintd's
design.

## A sign-in, step by step

1. A program authenticates through PAM, and `pam_kenning.so` runs before the password.
2. The module gives up at once (PAM falls through to the password) if:
   - the login manager is checking a typed password (`FACEUNLOCK_SKIP=1`);
   - face unlock is turned off in the config;
   - the request may come from somewhere else (see [Who counts as at the computer](#who-counts-as-at-the-computer));
   - the lid is closed.
3. It asks the service `has-model <user>`. The service answers:
   - 16 if the user hasn't signed in with a password since boot (see [After a restart](#after-a-restart));
   - 14 if the user is locked out;
   - 10 if they have no face model.

   The module gives up on any of these, saying why.
4. It asks `compare <user>`. The service opens the camera and looks for up to 4 seconds
   (`video/timeout`). For each face found (YuNet), it computes a feature vector (SFace)
   and compares it with the user's saved ones. A score of (1 − cosine similarity) × 10
   at or below 6.0 (`video/certainty`) is a match.
5. A match returns `PAM_SUCCESS`, and the caller signs you in. Anything else returns
   failure or "unavailable", and PAM asks for the password.

## Fingerprints

fprintd reads the finger; Kenning decides where it may be used, as for the face.

- `pam_kenning.so fingerprint` asks the service `fingerprint <user>`. The service claims
  fprintd's default reader for the user, passes on "Place your finger…" and retries, and
  gives up after `fingerprint/max_tries` fingers that weren't the user's. Each counts
  towards a rate limit of its own (`/run/kenning/fingerprint-failures`).
  VerifyStatus signals are only believed from fprintd's own bus connection.
- The service has no session, so fprintd would refuse it. A polkit rule
  (`50-kenning.rules`) lets the `kenning` user verify any user's finger, never enrol or
  delete one, as pam_fprintd running as root may. The service only asks for root or for
  the user's own account.
- **Lock screen (Plasma 6.8):** kscreenlocker runs `kde-fingerprint` twice: as the
  Fingerprint method when chosen, and unasked beside every other method, which is what
  made fingerprints part of the password prompt. Kenning's `kde-fingerprint` uses
  `pam_kenning.so fingerprint explicit`, which first asks the PAM conversation a question
  (`kenning-start-fingerprint`). Plasma never passes on prompts from the unasked run, so
  that one waits until the lock screen closes; the lock screen add-on answers it for the
  chosen method. pam-tidy only installs this while the add-on is applied.
- **Login screen:** the greeter's Use Fingerprint button sends the method with the login
  request; the helper sets `KENNING_METHOD=fingerprint` in the PAM environment.
- Fingerprints don't wait for a password after a restart: the first sign-in may use one.
- Where the distribution puts `pam_fprintd.so` in a shared stack (Fedora's `system-auth`,
  Debian's `common-auth`), pam-tidy writes its own `kde` and `polkit-1` with the includes
  inlined and `pam_fprintd.so` taken out, so the password never waits for a finger. The
  distribution's files are kept and put back.
- `sudo` has no buttons to choose with. `pam-tidy sudo-fingerprint on` adds
  `pam_kenning.so fingerprint` after the face line, if there is one, and in front of the
  password (inlining `sudo` as above where its stack reaches `pam_fprintd.so`). In this
  mode the module asks only with a terminal (`PAM_TTY`), so a password piped to `sudo -S`
  never waits for the reader; asks the service `has-fingerprint` first, so a user without
  one, or locked out, goes straight to the password; and catches Ctrl+C while the reader
  waits, which moves on to the password instead of ending `sudo` (also when the program
  holds SIGINT back during PAM).

## Administrator prompts

polkit's helper uses one PAM service, `polkit-1`, so a method of its own needs the dialog
to take part. Kenning's dialog shows Password, Fingerprint and Face:

1. Choosing Fingerprint or Face, the dialog asks the service `grant <user> <method>`.
   Only the user's own account may ask, only for a method switched on in `[polkit]`, and
   with `abort_if_ssh`, only from a local session or for a user signed in at this computer.
2. On a match the service answers with a random 128-bit token, kept for 15 seconds.
3. The dialog starts polkit's PAM conversation again. `pam_kenning.so grant`, first in
   `polkit-1`, asks the service `has-grant` (root only); only then does it ask the
   conversation for the token, which it checks with `use-grant` (root only). Any attempt
   uses the token up.
4. Without a pending token the line passes at once, so the password prompt never waits
   and other polkit agents (pkexec's) never see the question.

`run0` and `pkexec` are polkit requests like any other and get the same dialog. The dialog
shows a button for each method that is switched on and set up (`has-fingerprint`,
`has-model`), also while it can't be used: locked out, the camera away, or the face
waiting for a password after a restart. It starts with the preferred method if that one
is usable, else with the other, and a method chosen while unusable says why and returns
to the password.

## Trust boundaries

**The socket** is world-writable (`0666`), so any local program can connect. The
service reads the caller's identity from the kernel (`SO_PEERCRED`) and answers only:
- root, which is how the login manager, `sudo`, polkit's helper and the settings helper ask;
- the user asking about their own face or finger.

Set Up (`enroll`) and the polkit token checks (`has-grant`, `use-grant`) are root only;
asking for a token (`grant`) is for the user's own account only.

A request about another user is refused (code 12), except `password-first <user>`: the
login screen asks it so it can go straight to the password after a restart, and it only
says whether that user has signed in since boot, which `last` shows anyone anyway. It reads one short line per
connection and serves each in a thread of its own (at most 32 at once); a client that
says nothing is dropped after 2 seconds. Scans queue for the camera, and fingers for the
reader, in the order they came, with at most one waiting or running per user (root is not
limited).

**The service** runs in a tight systemd sandbox:
- no network devices at all (`PrivateNetwork=yes`), and only Unix sockets (which the
  system bus, for fprintd, is);
- no capabilities, and a read-only system except the face models;
- no home directories;
- only video devices.

A bug in OpenCV's image handling would be contained there.

**The face models** are in `/etc/kenning/models/<user>.dat`. The directory is `0700`
and both it and the files belong to `kenning`. Each holds the SFace feature vectors
(128 numbers per face) with a label and a time. No pictures are kept.

**The settings helper** runs as root, but never runs OpenCV: Set Up asks the service to
capture the face in its sandbox, and the service saves the model it owns. Each action is a
polkit action:
- listing faces needs nothing and returns only labels and times, never the vectors;
- adding or removing faces, changing settings and themes need an administrator password.

It validates every setting against a fixed list of keys and value ranges, and checks
theme archives before unpacking: at most 64 MB, 1,000 files and 16 levels of
folders, only plain files and folders (no links), and no absolute paths or `..`.

**The login and lock screens** decide nothing. They show the scan from PAM's
informational messages, so a wrong message could only change what's displayed.

## Rules

### Who counts as at the computer

The camera sees whoever sits in front of it, so a request must come from that person.
The module refuses (`abort_if_ssh`, on by default) when:
- PAM was given a remote host;
- the environment shows an SSH login;
- logind says the caller's session is remote, or can't tell;
- the caller is outside any session (as programs started by the user's service manager
  are on Plasma 6) and isn't root, and the user has no active, local graphical session.

### After a restart

Like a phone, face unlock waits for one sign-in by another method after each boot
(`password_after_boot`, on by default). The service allows a user only once logind
reports a local session of class `user` for them. The login screen can only create
one with the password, a fingerprint or automatic login, because it refuses face
unlock until then.

Users seen signed in are remembered in `/run/kenning/signed-in`, so signing out
doesn't undo it, and `/run` is emptied at every boot. `sudo` and the lock screen always
run inside a session, so they are never affected. The login screen asks
`password-first <user>` before its automatic scan and skips the scan while the answer
is 16, so after a restart it opens on the password box.

### Rate limit

Like `pam_faillock`: 5 faces that were seen and not recognized within 15 minutes lock
face unlock for that user for 10 minutes. Frames with no face, or too dark or bright,
don't count. The record is kept in `/run/kenning/failures`, so restarting the service
doesn't clear it.

### Ordinary webcams

An IR camera sees a photo or a screen very differently from a face. An ordinary webcam
doesn't. With a webcam:
- Set Up warns about this;
- `sudo` is left out by default;
- the README tells users to keep it off administrator prompts.

## Known weaknesses and open questions

These are where review would help most.

- **No liveness check.** On a webcam, a photo or video can get in. Blink and head-turn
  checks were tried and rejected; ROADMAP.md explains why.
- **Accuracy hasn't been measured** beyond the developer's laptop: false accepts and
  false rejects across people, cameras, glasses and lighting are unknown. The 6.0
  threshold (cosine similarity 0.4) is slightly stricter than the 0.363 in OpenCV's
  SFace sample, and was tested on one person.
- **A polkit token isn't tied to one request.** It is tied to the dialog that got it (only
  that dialog knows it) and lasts 15 seconds. PAM modules in polkit's helper can't see
  which request they are checking, so the token can't name one.
- **The lock screen's Fingerprint method relies on the add-on.** Without it the method
  would wait forever (fails safe: the password works); pam-tidy then hands fingerprints
  back to Plasma. Plasma running `kde-fingerprint` unasked is worth an upstream option.
- **The service may verify any user's finger** through fprintd (its polkit rule). It only
  does for root or the user's own account.
- **Signed in at the computer and over SSH at once:** a program started from the SSH
  login outside any session passes the "at the computer" rule, because the user has a
  local session, and the camera answers with whoever sits there.
- **The world-writable socket** relies entirely on the kernel's peer credentials and
  the service's request parsing.
- **Automatic login** counts as signing in after a restart.
- **The lock screen add-on** patches Plasma's own lock screen files on Plasma before
  6.8, and the login screen needs a patched Plasma Login Manager. Both are replaced
  when Plasma updates and re-applied by package hooks.
- **Root or the disk can read face models.** They're feature vectors, not pictures,
  but they are biometric data.

## Code map

| Where | What |
|---|---|
| `engine/native/src/pam_kenning.cpp` | the PAM module and the "at the computer" rule |
| `engine/native/src/service.h` | the socket protocol |
| `engine/native/src/daemon.cpp` | the service: who may ask, after a restart, rate limit |
| `engine/native/src/engine.cpp`, `pipeline.cpp` | camera, detection, recognition |
| `engine/native/src/fingerprint.cpp` | fingerprints through fprintd |
| `engine/native/src/grants.h` | polkit tokens |
| `engine/native/polkit/` | the rule that lets the service ask fprintd |
| `polkit-agent/` | Kenning's polkit dialog |
| `engine/native/systemd/` | the service's sandbox |
| `kcm/src/auth/` | the root helper and its polkit actions |
| `kcm/src/common/settingsschema.cpp` | accepted settings and their ranges |
| `packaging/pam-tidy` | how `pam_kenning.so` is added to and removed from PAM files |
| `ROADMAP.md` | decisions with their measurements |
