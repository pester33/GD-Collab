# GD Collab (Geode mod)

Live collab editing for Geometry Dash: host a room in the editor, friends join with the code, and everyone's edits sync.

## Download
- **GD 2.2091** (Geode 6.0.0): https://github.com/pester33/GD-Collab/releases/download/latest-build-2.2091/pester.gd-collab.geode
- **GD 2.2081** (Geode 5.11.2): https://github.com/pester33/GD-Collab/releases/download/latest-build/pester.gd-collab.geode

## Server setup (once)
Rooms and edits go through Supabase. Run `sql/setup.sql` once in the Supabase SQL Editor.

## How syncing works
Every object gets a shared id. About once a second each player's mod compares the editor with what it saw last time,
sends what was added / changed / deleted to `collab_events`, and applies everyone else's events. The newest event wins
when two people change the same object. The host saves a full copy of the level to the room every ~90 seconds so
people who join later load fast.
