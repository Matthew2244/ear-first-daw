# Contributing to Ear First

Thanks for helping. Ear First is built for people who work by ear, so the most useful contributions often come from screen reader users: tell us what you heard, what you expected, and what got in your way.

## Reporting a problem

Open an issue and say what you pressed, what the app said, and what you expected it to say. Your screen reader and system (VoiceOver on macOS 26, NVDA 2025 on Windows 11) help a lot.

## Changing code

- Every feature works by ear as well as by eye. If it only works on screen, it isn't done.
- Every action is a command in the command list, with a stable id, a plain description, and a spoken result.
- Never move a shipped default key. Add a new one instead.
- Keep spoken sentences short, and put the new state or the answer first.
- Add or update a test program for what you changed, and run them all before you send it.

## Licence of contributions

By sending a contribution, you agree that it is licensed under the GNU Affero General Public License, version 3, the same as the rest of Ear First, and that the project's maintainer, Matthew Whitaker, may also license it under other terms. That keeps the door open to offering Ear First under a commercial licence in the future, alongside the open source one.
