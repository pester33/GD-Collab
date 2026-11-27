# v1.2.4
- Cursors and edit flashes no longer live inside GD's object layer (safer with other editor mods like Editor Tab API)

# v1.2.3
- Fixed old copies of objects staying behind when someone edits them off screen (typing text left h, he, hel, hello)

# v1.2.2
- PC: your cursor now shows where you last placed or moved a block (mouse tracking didn't work on PC)

# v1.2.1
- Renamed to GD Gaming Collab
- New logo

# v1.2.0
- Color channels sync (BG, Ground, Line, 1-999, copy color, blending, opacity)
- People joining later get the room's colors too
- Groups sync with the objects (Edit Group changes are sent like any other edit)

# v1.1.1
- Fixed edits not syncing at all (it kept sending empty updates and never sent your blocks)
- Changes that fail to send are now sent again

# v1.1.0
- Collab button in My Levels: join a room with a code, or start a new collab level
- See everyone's cursor and name live in the editor
- Objects other people place, edit or delete flash in their color with their name
- Activity feed under the status (like "Pester placed 3, deleted 1")
- Needs sql/update-cursors.sql run once on the database for cursors

# v1.0.2
- Fixed crashing when joining a room with a big level
- Joining loads the room's level in one go (much faster)

# v1.0.1
- Moved the Collab button so it doesn't cover the Settings button

# v1.0.0
- First release: host/join rooms in the editor, live object sync, online list
