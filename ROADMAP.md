# Roadmap

In order. Each step builds on the one before.

## 1. Choose the engine: Howdy or native — decided: native

The native engine (C++ on OpenCV's YuNet and SFace, official packages only) is
now `master`. Howdy 3 vendored (needs python-dlib from the AUR) is kept on
`archive/howdy-bundled` as a fallback; it lacks fixes made after the decision,
such as skipping the face scan when a password was typed at the login screen.

Measured on 2026-10-30 (Asus Vivobook 16, 640x360 GREY IR camera, same seat,
alternating runs): native compare 1.65-1.68 s and recognised every time;
Howdy compare 1.91-1.97 s, with 2 of 6 runs timing out at 5.8 s. Both accept
the laptop looking up at the face (native scored 1.24 straight on, 3.11 at an
angle; 6.0 passes). About 1 s of every scan is the camera producing its first
frame (raw V4L2 measures the same), so it is the floor for either engine. The
earlier impression that native was slower came from a setup where PAM was
still using Howdy.

- [x] Add a timing breakdown to `kenning test`: time to the first lit frame,
      frames with a detected face, time to a match
- [x] Compare both: native was faster and never failed in the runs above;
      it needs no AUR packages, which suits KDE upstream
- [x] Merge the chosen branch into `master`; keep the other as an archive branch
- [x] Shave the remaining time: the networks load while the camera opens and
      stay loaded in the service. That takes ~50 ms of work out of each scan, but
      scans vary by about as much from run to run (1.50-1.64 s). What is left is the
      camera: ~220 ms to wake it from USB suspend (format hints to OpenCV don't
      help) and ~1 s until the IR sensor delivers its first frame. Keeping the
      camera awake would cost battery all day, so it stays as it is

## 2. Warning labels for risky options

Show a short warning (Kirigami.InlineMessage or a ContextualHelpButton) where an
option trades security or privacy for convenience:

- [x] Administrator prompts and sudo: your face can approve changes to the system
- [x] Match threshold set lenient: people who look like you may get in
- [x] "Don't use when the lid is closed" or "Don't use over SSH" turned off
- [x] Dismiss the lock screen automatically: unlocks as soon as you are seen,
      without a key press
- [x] "Say when face recognition starts" turned off: the login and lock screen
      animation depends on it

## 3. Laptops with only an ordinary (colour) webcam

Setup used to require an IR camera. Without one, it now offers the ordinary
webcam:

- [x] Setup finds the colour webcam when there is no IR camera and explains,
      with a warning to acknowledge, that it is less accurate, can be fooled by
      a photo of you and needs your face lit from the front. Virtual cameras
      (v4l2loopback) are never picked: any program can feed them a video
- [x] In that mode, default to the lock and login screens only; sudo and
      administrator prompts stay on the password (turning them on shows a red
      warning)
- [x] Try both approaches and keep the more reliable one. Measured on
      2026-10-30 with the Asus Vivobook 16's 720p webcam, enrolled and tested in
      the same light, scores (lower is better, 6 passes):
  - mimic IR (greyscale + CLAHE): 0.40-0.70
  - colour as-is: 0.16-0.20, and the fastest  ← chosen (`video/colour = true`)
  - colour + CLAHE: 0.13-0.55, 80 ms slower
  In dim light, against a face enrolled with the lights on, none of them found
  a face; lowering the detector threshold to 0.4 found one in 1-2 of ~100
  frames, scoring 6.9-10.8 (rejected). With a bright window behind the user no
  face was found either. Lighting is the limit, not the preprocessing.
- [ ] ~~Liveness check (blink or head turn)~~: decided against (2026-10-31). On
  the IR camera a blink spans at most one usable frame (the emitter lights every
  other frame, ~7 face frames/s, eyes ~38 px apart), glasses glare blanks an eye, and
  open/closed eye detail overlaps. A head turn would stop a held-up photo on a
  webcam but not a replayed video, and IR already rejects prints and screens. The
  answer stays: recommend IR, warn about webcams, keep webcams off sudo

## Follow-ups (not yet ordered)

- [x] Packages for Debian/Ubuntu and Fedora beside Arch, from one CMake install,
      built and install-tested in containers (Ubuntu 26.04 and 26.10, Fedora 44);
      the lock screen add-on keeps its own copies of the originals instead of
      using pacman's cache
- [x] Lock screen animation on Plasma 6.3-6.7 (Debian 13, Ubuntu 26.04 LTS): the
      add-on has edit sets for 6.3 and 6.6 beside 6.8, checked by loading the real
      greeter offscreen in CI (6.3.6, 6.6.6, 6.7.5, 6.8.2)
- [ ] Login screen face unlock beyond Arch. Done: patches per Plasma Login
      Manager version (login-manager/patches: 6.6 for Fedora 44's 6.6.4, 6.8),
      and login-manager/apply.sh picks the newest that applies without fuzz.
      Left: build plasma-login-manager-kenning for Fedora (and Debian/Ubuntu once
      they ship Plasma Login Manager), ideally by rebuilding the distribution's
      own package with the patch; check in CI that a patch fits each one
- [x] Plug and play: install/upgrade drops pam_howdy lines once Howdy is gone,
      removal drops pam_kenning lines, and the first login after startup
      (uptime under 10 minutes) offers Set Up once if there is an IR camera
- [x] A "Try Face Again" button on the lock screen after face unlock fails
      (add-on patch 10): calls rescanFace(), hidden while a password is typed,
      during a lockout and without a face model. Plasma 6.8 and newer only
      (patch 14): before 6.8 the rescan may not start until !388 lands, so the
      button would do nothing; the failure reason still shows there
- [x] The same button on the login screen (plasma-login-manager-kenning): runs
      the empty-password attempt again
- [x] ~~Photos of failed attempts~~: added in 0.7.0, removed after release
      (2026-11-01). It photographed anyone who wasn't recognized, including
      people in public who never agreed to it, and kept face photos that root
      or anyone holding the disk could read. Encrypting them wouldn't help: the
      service must save them while the user is away, so the key sits on the
      same machine. Package upgrades delete /var/lib/kenning/snapshots.
- [x] A root service that owns the camera and face models (like fprintd), so
      models no longer have to be readable by local users: `kenning daemon`,
      socket-activated, answering only for root or the user's own account
- [x] Use Plasma 6.8's own Face sign-in method (PAM service `kde-face`,
      `[Authenticators] Face=true`, which had no backend): pam-tidy writes
      `kde-face` from the distribution's `kde-fingerprint`, failing closed (a
      `pam_deny.so` stands behind `pam_kenning.so`, since kde-fingerprint's
      stack without its module line lets anyone in), and moves between it and
      the `kde` service when Plasma is upgraded or downgraded. Face is switched
      on per user (System Settings, or at login for users with a face model).
      Lock screen add-on patch 7 starts with Face, falls back to the password on
      a failure instead of Plasma's immediate retry, and switches to the
      password when you type
- [ ] Upstream route, still to do: the login manager auto-scan as a merge
      request, the settings page as a KCM or part of Users
- [ ] kscreenlocker fixes to send: (1) a remembered method that has since been
      switched off crashes the lock screen until KWin gives up ("loginctl"
      screen): bug 528042, merge request !387 with an autotest. Kenning never
      switches Face off without forgetting a remembered Face.
      (2) a method started before its PAM worker is ready isn't retried until
      a key or mouse wakes the screen (the add-on retries for fingerprint;
      being fixed in !352).
      (3) a real preferred-method setting, and starting fingerprint only after
      face fails: done in the add-on (preferredMethod, fingerprintFallback); only
      upstreaming is left, so the workaround can go
      (4) after stopAuthenticating(), startAuthenticating() doesn't start a
      new PAM worker until the method changes (!388, under review). The
      add-on's rescan stops and starts the password method where face unlock
      lives in the kde service (Plasma without kde-face), so it may not rescan
      there until !388 lands
- [x] Rate limit for face attempts (like pam_faillock): the service locks a
      user's face unlock for lockout_minutes after max_failures faces that were
      seen but not recognized within failure_window minutes (config.ini;
      5 / 15 / 10 by default, 0 turns it off). Nobody in front of the camera,
      darkness or a scan cancelled by typing doesn't count. The record is kept
      in /run/kenning/failures (like pam_faillock's in /run), so a crash or
      restart of the service doesn't reset it; a reboot does
- [x] Guided enrolment: Set Up asks for looking straight, then turning left,
      right, up and down, and keeps encodings for each angle (the head pose comes
      from the detector's landmarks; a turn that isn't seen in time is skipped).
      It asks to look back at the camera between turns, rejects a turn that
      drifts on the other axis (with a hint), and shows an animated head and a
      dot per step.
      Afterwards it offers a second look with glasses taken off or put on.
      It starts only when the person confirms they're ready, after a 3-second
      countdown; the steps are read aloud (on by default, a checkbox; needs
      speech-dispatcher, otherwise the checkbox says so). The preview shows the
      ordinary webcam (a virtual one is fine for that) while the IR camera scans
      Learning from successful unlocks was decided against (2026-10-31): each
      learned sample widens what gets accepted, a slow drift towards whoever
      unlocks; deliberate captures cover the same changes
- [x] Preferred sign-in method when a fingerprint is enrolled (System Settings,
      Lock screen): face first (fingerprint and password after a face failure),
      or fingerprint first (face and password when it fails or times out), or
      the method used last. Fingerprint runs alongside face from the start
      until (3) above lands
- [x] A trademark-safe product name: **Kenning** (to ken = to know, recognise),
      in KDE's style of real words with a K. Quick web checks found no clash in
      face recognition or authentication; KFace (KDE's archived libkface),
      KVision (Kotlin framework), KGlance (Glance face unlock) and Kognize (close
      to the KOGNIZA trademark application) were ruled out. A formal trademark
      search (USPTO, EUIPO) is still needed before a commercial release
- [x] Password first after a restart (2026-11-01, `[core] password_after_boot`,
      on by default), like phones: the service refuses face unlock (code 16) for
      a user with no local logind session of class "user" since boot, so the
      first sign-in needs the password or a fingerprint. Users seen signed in
      are kept in /run/kenning/signed-in, so signing out doesn't undo it and a
      reboot does. No PAM changes: sudo and the lock screen always run inside a
      session. Autologin counts as signed in. Not yet: also asking again after
      48 hours without the password, as phones do
- [x] Set Up captures faces in the sandboxed service (2026-11-08): root (the
      settings helper, sudo kenning add) asks "enroll <user> <label>", the service
      captures and saves the model it owns, so OpenCV never runs as root
- [x] The service serves each connection in a thread (2026-11-08): a scan, or a
      client that says nothing, no longer holds up everyone else; scans queue for
      the camera, at most one per user
- [x] Fingerprints as a choice of their own (2026-11-08), through fprintd, with
      Kenning's rate limit and messages: the lock screen's Fingerprint method reads
      the finger only when chosen (Plasma 6.8 runs kde-fingerprint unasked beside
      every method; Kenning's waits on a prompt only the add-on answers), Kenning's
      polkit dialog offers Password, Fingerprint and Face with a one-time token
      for pam_kenning.so grant, and the login screen has a Use Fingerprint button.
      On Fedora and Debian, kde and polkit-1 stop waiting for a finger before the
      password
- [x] A finger for sudo in the terminal (2026-11-11): after the face and before the
      password, only at a terminal, with Ctrl+C on to the password. Kenning's polkit
      dialog (also what run0 and pkexec show) keeps the button of a method that can't
      be used right now, and says why
- [ ] Upstream for fingerprints: a kscreenlocker option not to run kde-fingerprint
      unasked (then the add-on's prompt can go), and methods in polkit-kde-agent-1
      (then Kenning's dialog can go)
- [ ] Measured recognition accuracy before any stronger security claim:
      false accepts and false rejects across cameras (IR and webcam), lighting,
      glasses, several people and attack types (photos, screens, look-alikes),
      from testers on many devices, not just the developer's laptop
- [ ] End-to-end tests of signing in: PAM stack, socket activation, the real
      greeter and retries on each supported Plasma and distribution, with a
      fake camera (v4l2loopback feeding recorded frames) in the containers
- [ ] All-or-nothing settings save: the helper writes config.ini, the PAM
      files and kenningrc in turn and stops at the first error, which can leave
      part of a save applied (reported as an error; Apply again finishes it)
