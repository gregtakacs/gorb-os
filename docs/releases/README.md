# Release notes

Each tag has a file here, `<tag>.md` (`v1.2.11.md`, `v1.2.11-2.md`), written for people
deciding whether to update. It goes at the top of the release's notes;
`tools/release_notes.sh` adds the template below it (the firmware and apps built in, the
downloads, the signing warnings) and folds every commit subject since the last
*published* release under the end. The release workflow refuses to build a tag that has
no file here.

Nothing is lost to a release that is never published. A tag whose draft was thrown away,
or that was rebuilt as `-2`, keeps its file, and the next release that is published
carries those notes under "Also in <tag>", with every commit since the last release
owners actually got. So write each tag's file for its own changes only.

Write it before tagging, and preview the whole thing with:

```sh
tools/release_notes.sh v1.2.11
```

What goes in, in this order, and only what applies:

- **What's new**: what an owner will notice, in their words (the knob, the screens, the
  hub), not the code's.
- **Changes**: things that now work differently, so nobody is surprised.
- **Fixed**: bugs an owner could have hit.
- **Before you update**: anything to do first, or a step the update needs (an app that
  needs this firmware, a setting that moved).

Keep it short: the full list of commits is already underneath.
