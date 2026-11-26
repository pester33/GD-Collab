-- GD Collab v1.1: live cursors. Run once in the Supabase SQL Editor (safe to run again).
alter table public.collab_members add column if not exists cursor_x real not null default 0;
alter table public.collab_members add column if not exists cursor_y real not null default 0;
alter table public.collab_members add column if not exists has_cursor boolean not null default false;
