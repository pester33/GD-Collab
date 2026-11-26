-- GD Collab: live collab editing rooms
-- Run once in Supabase: Dashboard -> SQL Editor -> New query -> paste -> Run.
-- Safe to run again.

-- Rooms: one per collab session. `snapshot` is the full level (all objects) at `snapshot_event`.
create table if not exists public.collab_rooms (
  code           text primary key check (code ~ '^[A-Z0-9]{4,8}$'),
  host           text not null default '',
  level_name     text not null default '',
  snapshot       text not null default '[]',
  snapshot_event bigint not null default 0,
  created_at     timestamptz not null default now(),
  updated_at     timestamptz not null default now()
);

-- Changes: every batch of edits someone made (objects added / changed / deleted).
create table if not exists public.collab_events (
  id         bigint generated always as identity primary key,
  room       text not null references public.collab_rooms(code) on delete cascade,
  client     text not null,
  author     text not null default '',
  data       text not null,
  created_at timestamptz not null default now()
);
create index if not exists collab_events_room_id on public.collab_events (room, id);

-- Who's in a room right now (each player pings every few seconds).
create table if not exists public.collab_members (
  room      text not null references public.collab_rooms(code) on delete cascade,
  client    text not null,
  name      text not null default '',
  last_seen timestamptz not null default now(),
  primary key (room, client)
);

alter table public.collab_rooms   enable row level security;
alter table public.collab_events  enable row level security;
alter table public.collab_members enable row level security;

-- Anyone with the mod can use rooms; knowing the room code is what lets you in.
drop policy if exists "rooms read"    on public.collab_rooms;
drop policy if exists "rooms create"  on public.collab_rooms;
drop policy if exists "rooms update"  on public.collab_rooms;
create policy "rooms read"   on public.collab_rooms for select using (true);
create policy "rooms create" on public.collab_rooms for insert with check (true);
create policy "rooms update" on public.collab_rooms for update using (true) with check (true);

drop policy if exists "events read"   on public.collab_events;
drop policy if exists "events add"    on public.collab_events;
create policy "events read" on public.collab_events for select using (true);
create policy "events add"  on public.collab_events for insert with check (length(data) < 5000000);

drop policy if exists "members read"   on public.collab_members;
drop policy if exists "members join"   on public.collab_members;
drop policy if exists "members ping"   on public.collab_members;
create policy "members read" on public.collab_members for select using (true);
create policy "members join" on public.collab_members for insert with check (true);
create policy "members ping" on public.collab_members for update using (true) with check (true);

grant select, insert, update on public.collab_rooms, public.collab_members to anon, authenticated;
grant select, insert on public.collab_events to anon, authenticated;

-- Clean up rooms nobody touched for 3 days (runs whenever someone creates a room)
create or replace function public.collab_cleanup() returns trigger
language plpgsql security definer set search_path = public as $$
begin
  delete from collab_rooms where updated_at < now() - interval '3 days';
  return new;
end;
$$;
drop trigger if exists collab_cleanup_on_create on public.collab_rooms;
create trigger collab_cleanup_on_create after insert on public.collab_rooms
  for each statement execute function public.collab_cleanup();

-- Keep a room "alive" while people are editing in it
create or replace function public.collab_touch_room() returns trigger
language plpgsql security definer set search_path = public as $$
begin
  update collab_rooms set updated_at = now() where code = new.room;
  return new;
end;
$$;
drop trigger if exists collab_touch_on_event on public.collab_events;
create trigger collab_touch_on_event after insert on public.collab_events
  for each row execute function public.collab_touch_room();
alter table public.collab_members add column if not exists cursor_x real not null default 0;
alter table public.collab_members add column if not exists cursor_y real not null default 0;
alter table public.collab_members add column if not exists has_cursor boolean not null default false;
