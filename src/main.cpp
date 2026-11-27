#include <Geode/Geode.hpp>
#include <Geode/modify/LevelEditorLayer.hpp>
#include <Geode/modify/EditorPauseLayer.hpp>
#include <Geode/modify/MenuLayer.hpp>
#include <Geode/modify/LevelBrowserLayer.hpp>
#include <Geode/modify/EditorUI.hpp>
#include <Geode/ui/TextInput.hpp>
#include <Geode/ui/BasedButtonSprite.hpp>
#include <Geode/utils/web.hpp>
#include <Geode/utils/async.hpp>
#include <Geode/utils/random.hpp>

#include <chrono>
#include <deque>
#include <map>
#include <unordered_map>
#include <unordered_set>

using namespace geode::prelude;

// Geode 6 (GD 2.2091) and Geode 5 (GD 2.2081) take different time types for timeouts
static void setTimeout(web::WebRequest& req, int secs) {
#if GEODE_COMP_GD_VERSION >= 22091
    req.timeout(asp::Duration::fromSecs(secs));
#else
    req.timeout(std::chrono::seconds(secs));
#endif
}

// Supabase project for collab rooms (publishable key: safe to ship, access is limited by RLS)
static constexpr auto SUPA_URL = "https://daofdapshrvvqwdlkqae.supabase.co";
static constexpr auto SUPA_KEY = "sb_publishable_q2xRu0jn0aG8VIKWCcvc8w_QpjBM7VT";

static web::WebRequest supa() {
    web::WebRequest req;
    req.header("apikey", SUPA_KEY);
    req.header("Content-Type", "application/json");
    setTimeout(req, 20);
    return req;
}

static std::string randomString(size_t len, std::string_view alphabet) {
    std::string out;
    for (size_t i = 0; i < len; i++) out += alphabet[random::generate<uint64_t>() % alphabet.size()];
    return out;
}

static std::string playerName() {
    std::string name = GameManager::get()->m_playerName;
    return name.empty() ? "Player" : name;
}

// days since 1970-01-01 -> civil date (Howard Hinnant)
static void civilFromDays(int64_t z, int& y, unsigned& m, unsigned& d) {
    z += 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    unsigned doe = static_cast<unsigned>(z - era * 146097);
    unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t yy = static_cast<int64_t>(yoe) + era * 400;
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    unsigned mp = (5 * doy + 2) / 153;
    d = doy - (153 * mp + 2) / 5 + 1;
    m = mp < 10 ? mp + 3 : mp - 9;
    y = static_cast<int>(yy + (m <= 2));
}

// UTC time as "2026-11-26T13:41:36Z", optionally shifted
static std::string isoNow(int64_t offsetSecs = 0) {
    int64_t t = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count() + offsetSecs;
    int64_t days = t / 86400, rem = t % 86400;
    if (rem < 0) { rem += 86400; days--; }
    int y; unsigned m, d;
    civilFromDays(days, y, m, d);
    return fmt::format("{:04}-{:02}-{:02}T{:02}:{:02}:{:02}Z", y, m, d, rem / 3600, (rem / 60) % 60, rem % 60);
}

// Every player gets their own color, picked from their client id
static ccColor3B colorForClient(std::string const& client) {
    static constexpr ccColor3B palette[] = {
        { 255, 95, 95 }, { 90, 200, 255 }, { 120, 255, 120 }, { 255, 205, 60 },
        { 215, 125, 255 }, { 255, 145, 205 }, { 80, 255, 220 }, { 255, 160, 80 },
    };
    uint32_t h = 2166136261u;
    for (unsigned char c : client) { h ^= c; h *= 16777619u; }
    return palette[h % (sizeof(palette) / sizeof(palette[0]))];
}

// ----- Color channels (BG, Ground, 1-999...) live in the level settings string under kS38
// kS38 looks like "1_255_2_0_3_0_6_1000_7_1|1_0_2_102_3_255_6_1001_7_1|..."
static std::string levelColorString(LevelEditorLayer* editor) {
    if (!editor->m_levelSettings) return "";
    std::string header = editor->m_levelSettings->getSaveString();
    auto start = header.find("kS38,");
    if (start == std::string::npos) return "";
    start += 5;
    auto end = header.find(',', start);
    return header.substr(start, end == std::string::npos ? std::string::npos : end - start);
}

static int colorEntryChannel(std::string const& entry) {
    // pairs of key_value: channel id is key 6
    auto parts = utils::string::split(entry, "_");
    for (size_t i = 0; i + 1 < parts.size(); i += 2) {
        if (parts[i] == "6") return utils::numFromString<int>(parts[i + 1]).unwrapOr(0);
    }
    return 0;
}

static std::map<int, std::string> splitColors(std::string const& str) {
    std::map<int, std::string> out;
    for (auto& entry : utils::string::split(str, "|")) {
        if (entry.empty()) continue;
        int id = colorEntryChannel(entry);
        if (id > 0) out[id] = entry;
    }
    return out;
}

// ---------------------------------------------------------------------------
// The collab session: tracks every object with a shared id and syncs changes
// ---------------------------------------------------------------------------

struct Tracked {
    std::string uuid;
    std::string str; // object's save string the last time we looked
    Ref<GameObject> keep; // keeps the object alive until we notice it's gone
};

class CollabSession {
public:
    static CollabSession& get() {
        static CollabSession instance;
        return instance;
    }

    bool active = false;
    bool isHost = false;
    std::string room;
    std::string status = "Not in a room";
    std::vector<std::string> members;

    // Other players in the room, with their cursor (in level coordinates)
    struct Member {
        std::string client;
        std::string name;
        float x = 0.f, y = 0.f;
        bool hasCursor = false;
        ccColor3B color;
    };
    std::vector<Member> others;

    // Objects someone else just changed, waiting to be flashed in the editor
    struct Highlight {
        std::vector<CCRect> rects;
        std::string name;
        ccColor3B color;
    };
    std::vector<Highlight> highlights;

    // "Pester placed 3, deleted 1"
    struct FeedLine {
        std::string text;
        ccColor3B color;
        float age = 0.f;
    };
    std::deque<FeedLine> feed;

    // Our own cursor, sent with every ping
    CCPoint cursor = { 0.f, 0.f };
    bool hasCursor = false;
    // where we last placed / moved an object (PC shows this as our cursor)
    CCPoint lastEditPos = { 0.f, 0.f };
    bool hasEditPos = false;

    bool isActiveFor(LevelEditorLayer* editor) const {
        return active && m_editor.lock().data() == editor;
    }

    // ----- Starting / stopping

    void host(LevelEditorLayer* editor, std::function<void(bool, std::string)> done, int attempt = 0) {
        this->reset();
        m_editor = editor;
        this->scanLocal(editor, true); // give every existing object an id (nothing is sent yet)

        auto code = randomString(5, "ABCDEFGHJKLMNPQRSTUVWXYZ23456789");
        std::string levelName = editor->m_level ? std::string(editor->m_level->m_levelName) : "";
        auto body = matjson::makeObject({
            { "code", code },
            { "host", playerName() },
            { "level_name", levelName },
            { "snapshot", this->snapshotJson() },
            { "snapshot_event", 0 },
        });
        auto req = supa();
        req.header("Prefer", "return=minimal");
        req.bodyJSON(body);
        m_tasks.spawn(req.post(fmt::format("{}/rest/v1/collab_rooms", SUPA_URL)), [this, done, attempt, code](web::WebResponse res) {
            if (res.code() == 409 && attempt < 5) {
                // code already taken, try another
                if (auto ed = m_editor.lock()) return this->host(ed, done, attempt + 1);
            }
            if (!res.ok()) {
                log::warn("[collab] Couldn't create room: HTTP {} {}", res.code(), res.string().unwrapOr(""));
                this->reset();
                return done(false, fmt::format("Couldn't create a room (HTTP {})", res.code()));
            }
            room = code;
            isHost = true;
            active = true;
            status = "Hosting";
            log::info("[collab] Hosting room {}", code);
            this->ping();
            done(true, "");
        });
    }

    void join(LevelEditorLayer* editor, std::string code, std::function<void(bool, std::string)> done) {
        this->reset();
        m_editor = editor;
        auto url = fmt::format("{}/rest/v1/collab_rooms?code=eq.{}&select=snapshot,snapshot_event,host,level_name", SUPA_URL, code);
        m_tasks.spawn(supa().get(url), [this, done, code](web::WebResponse res) {
            auto json = res.json().unwrapOr(matjson::Value());
            if (!res.ok() || !json.isArray() || json.size() == 0) {
                this->reset();
                return done(false, res.ok() ? "No room with that code" : fmt::format("Couldn't reach the server (HTTP {})", res.code()));
            }
            auto editor = m_editor.lock();
            if (!editor) return this->reset();

            auto snapshot = matjson::parse(json[0]["snapshot"].asString().unwrapOr("[]")).unwrapOr(matjson::Value());
            m_lastEvent = json[0]["snapshot_event"].asInt().unwrapOr(0);
            this->clearLevel(editor);
            std::vector<std::pair<std::string, std::string>> objects;
            std::vector<std::string> colorEntries;
            if (snapshot.isArray()) {
                for (auto& pair : snapshot) {
                    if (pair.isArray() && pair.size() == 2) {
                        auto uuid = pair[0].asString().unwrapOr("");
                        auto str = pair[1].asString().unwrapOr("");
                        if (uuid == "#colors") {
                            for (auto& [id, entry] : splitColors(str)) colorEntries.push_back(entry);
                        } else {
                            objects.push_back({ uuid, str });
                        }
                    }
                }
            }
            this->createObjects(editor, objects);
            this->applyColors(editor, colorEntries);
            m_colors = splitColors(levelColorString(editor)); // starting point: nothing to send yet
            room = code;
            isHost = false;
            active = true;
            status = fmt::format("Joined {}'s room", json[0]["host"].asString().unwrapOr("someone"));
            log::info("[collab] Joined room {} ({} objects, from event {})", code, objects.size(), m_lastEvent);
            this->ping();
            done(true, "");
        });
    }

    void leave() {
        if (active) log::info("[collab] Left room {}", room);
        this->reset();
    }

    // ----- Called about once a second by the editor

    void tick(LevelEditorLayer* editor) {
        if (!this->isActiveFor(editor)) return;
        // While playtesting, triggers move objects around: don't treat that as edits
        if (editor->m_playbackMode != PlaybackMode::Not) {
            status = "Paused while playtesting";
            return;
        }
        m_ticks++;

        // 1) send our own edits (one batch at a time)
        if (!m_pushing) {
            auto ops = this->scanLocal(editor, false);
            if (m_retry.size() > 0) {
                // a send failed earlier: send those changes again, plus the new ones
                auto all = std::move(m_retry);
                m_retry = matjson::Value(std::vector<matjson::Value>{});
                for (auto& op : ops) all.push(op);
                ops = std::move(all);
            }
            if (ops.size() > 0) this->push(std::move(ops));
        }
        // 2) pull in everyone else's edits (not while our own batch is on its way,
        //    so the newest change always wins the same way for everyone)
        if (!m_pushing && !m_polling) this->poll();
        // 3) who's here, and where everyone's cursor is
        if (!m_pinging) this->ping();
        // 4) the host keeps the room's saved copy fresh for people who join later
        if (isHost && m_ticks % 90 == 0 && !m_pushing) this->saveSnapshot(editor);
    }

private:
    WeakRef<LevelEditorLayer> m_editor;
    std::string m_client = randomString(8, "abcdefghijklmnopqrstuvwxyz0123456789");
    uint64_t m_counter = 0;
    int64_t m_lastEvent = 0;
    int64_t m_maxOwnEvent = 0;
    int m_ticks = 0;
    bool m_pushing = false;
    bool m_polling = false;
    bool m_pinging = false;
    bool m_cursorColumns = true; // false if the database wasn't updated for cursors yet
    std::unordered_map<GameObject*, Tracked> m_tracked;
    std::unordered_map<std::string, GameObject*> m_byUuid;
    std::unordered_map<std::string, int64_t> m_ownLatest; // uuid -> id of our newest event touching it
    std::map<int, std::string> m_colors; // color channel -> its settings the last time we looked
    async::TaskHolder<web::WebResponse> m_tasks;
    async::TaskHolder<web::WebResponse> m_pushTask;
    async::TaskHolder<web::WebResponse> m_pollTask;
    async::TaskHolder<web::WebResponse> m_pingTask;
    async::TaskHolder<web::WebResponse> m_snapTask;

    void reset() {
        active = false;
        isHost = false;
        room.clear();
        members.clear();
        others.clear();
        highlights.clear();
        feed.clear();
        hasCursor = false;
        hasEditPos = false;
        m_pinging = false;
        status = "Not in a room";
        m_editor = nullptr;
        m_tracked.clear();
        m_byUuid.clear();
        m_ownLatest.clear();
        m_colors.clear();
        m_lastEvent = 0;
        m_maxOwnEvent = 0;
        m_ticks = 0;
        m_pushing = false;
        m_polling = false;
        m_retry = matjson::Value(std::vector<matjson::Value>{});
        m_tasks.cancel();
        m_pushTask.cancel();
        m_pollTask.cancel();
        m_pingTask.cancel();
        m_snapTask.cancel();
    }

    std::string newUuid() {
        return fmt::format("{}{:x}", m_client.substr(0, 5), ++m_counter);
    }

    static std::string saveString(LevelEditorLayer* editor, GameObject* obj) {
        return std::string(obj->getSaveString(editor));
    }

    // Compares the level with what we saw last time. Returns the changes as ops.
    matjson::Value scanLocal(LevelEditorLayer* editor, bool silent) {
        std::vector<matjson::Value> ops;
        std::unordered_set<GameObject*> seen;
        if (editor->m_objects) {
            for (auto obj : CCArrayExt<GameObject*>(editor->m_objects)) {
                if (!obj) continue;
                seen.insert(obj);
                auto str = saveString(editor, obj);
                auto it = m_tracked.find(obj);
                if (it == m_tracked.end()) {
                    auto uuid = this->newUuid();
                    m_tracked[obj] = { uuid, str, obj };
                    m_byUuid[uuid] = obj;
                    if (!silent) {
                        ops.push_back(matjson::makeObject({ { "o", "a" }, { "u", uuid }, { "s", str } }));
                        lastEditPos = obj->getPosition();
                        hasEditPos = true;
                    }
                } else if (it->second.str != str) {
                    it->second.str = str;
                    if (!silent) {
                        ops.push_back(matjson::makeObject({ { "o", "m" }, { "u", it->second.uuid }, { "s", str } }));
                        lastEditPos = obj->getPosition();
                        hasEditPos = true;
                    }
                }
            }
        }
        for (auto it = m_tracked.begin(); it != m_tracked.end();) {
            if (!seen.contains(it->first)) {
                if (!silent) ops.push_back(matjson::makeObject({ { "o", "d" }, { "u", it->second.uuid } }));
                m_byUuid.erase(it->second.uuid);
                it = m_tracked.erase(it);
            } else {
                ++it;
            }
        }
        auto colors = splitColors(levelColorString(editor));
        for (auto& [id, entry] : colors) {
            auto it = m_colors.find(id);
            if (it != m_colors.end() && it->second == entry) continue;
            m_colors[id] = entry;
            if (!silent) ops.push_back(matjson::makeObject({ { "o", "c" }, { "u", fmt::format("#c{}", id) }, { "s", entry } }));
        }
        return matjson::Value(ops); // empty array = no changes
    }

    std::string snapshotJson() {
        std::vector<matjson::Value> list;
        list.reserve(m_tracked.size());
        for (auto& [obj, t] : m_tracked) list.push_back(matjson::Value(std::vector<matjson::Value>{ t.uuid, t.str }));
        if (auto editor = m_editor.lock()) {
            list.push_back(matjson::Value(std::vector<matjson::Value>{ "#colors", levelColorString(editor) }));
        }
        return matjson::Value(list).dump(matjson::NO_INDENTATION);
    }

    void push(matjson::Value ops) {
        m_pushing = true;
        std::vector<std::string> uuids;
        for (auto& op : ops) uuids.push_back(op["u"].asString().unwrapOr(""));
        auto body = matjson::makeObject({
            { "room", room },
            { "client", m_client },
            { "author", playerName() },
            { "data", ops.dump(matjson::NO_INDENTATION) },
        });
        auto req = supa();
        req.header("Prefer", "return=representation");
        req.bodyJSON(body);
        auto count = ops.size();
        m_pushTask.spawn(req.post(fmt::format("{}/rest/v1/collab_events?select=id", SUPA_URL)), [this, uuids, count, ops](web::WebResponse res) {
            m_pushing = false;
            if (!res.ok()) {
                log::warn("[collab] Send failed: HTTP {} {}", res.code(), res.string().unwrapOr("").substr(0, 200));
                status = "Connection problem, retrying...";
                // put the changes back so they're sent next time
                m_retry = ops;
                return;
            }
            auto json = res.json().unwrapOr(matjson::Value());
            int64_t id = json.isArray() && json.size() > 0 ? json[0]["id"].asInt().unwrapOr(0) : 0;
            for (auto& u : uuids) m_ownLatest[u] = id;
            m_maxOwnEvent = std::max(m_maxOwnEvent, id);
            status = fmt::format("Sent {} change{}", count, count == 1 ? "" : "s");
            if (m_retry.size() > 0) {
                auto retry = std::move(m_retry);
                m_retry = matjson::Value(std::vector<matjson::Value>{});
                this->push(std::move(retry));
            }
        });
    }
    matjson::Value m_retry = matjson::Value(std::vector<matjson::Value>{}); // note: matjson::Value() is an empty OBJECT, not null

    void poll() {
        m_polling = true;
        auto url = fmt::format(
            "{}/rest/v1/collab_events?room=eq.{}&id=gt.{}&client=neq.{}&select=id,client,author,data&order=id.asc&limit=200",
            SUPA_URL, room, m_lastEvent, m_client
        );
        m_pollTask.spawn(supa().get(url), [this](web::WebResponse res) {
            m_polling = false;
            auto editor = m_editor.lock();
            if (!editor || !active) return;
            if (!res.ok()) {
                status = "Connection problem, retrying...";
                return;
            }
            auto json = res.json().unwrapOr(matjson::Value());
            if (!json.isArray()) return;
            if (editor->m_playbackMode != PlaybackMode::Not) return; // apply after playtest
            int applied = 0;
            std::string lastAuthor;
            for (auto& ev : json) {
                int64_t id = ev["id"].asInt().unwrapOr(0);
                auto ops = matjson::parse(ev["data"].asString().unwrapOr("[]")).unwrapOr(matjson::Value());
                lastAuthor = ev["author"].asString().unwrapOr("Someone");
                auto client = ev["client"].asString().unwrapOr("");
                if (ops.isArray()) applied += this->applyOps(editor, ops, id, lastAuthor, client);
                m_lastEvent = std::max(m_lastEvent, id);
            }
            if (applied > 0) status = fmt::format("{} edited {} object{}", lastAuthor, applied, applied == 1 ? "" : "s");
        });
    }

    // Applies someone else's changes. Newest event wins: skip anything we changed more recently.
    int applyOps(LevelEditorLayer* editor, matjson::Value const& ops, int64_t eventId, std::string const& author, std::string const& client) {
        std::vector<std::pair<std::string, std::string>> toCreate;
        std::vector<CCRect> rects;
        std::vector<std::string> colorEntries;
        int count = 0, placed = 0, edited = 0, deleted = 0;
        for (auto& op : ops) {
            auto kind = op["o"].asString().unwrapOr("");
            auto uuid = op["u"].asString().unwrapOr("");
            if (uuid.empty()) continue;
            auto own = m_ownLatest.find(uuid);
            if (own != m_ownLatest.end() && own->second > eventId) continue;

            if (kind == "c") {
                colorEntries.push_back(op["s"].asString().unwrapOr(""));
                count++;
                continue;
            }
            if (auto existing = m_byUuid.find(uuid); existing != m_byUuid.end()) {
                if (kind == "d") rects.push_back(existing->second->getObjectRect());
                this->removeTracked(editor, existing->second);
            }
            if (kind == "a" || kind == "m") toCreate.push_back({ uuid, op["s"].asString().unwrapOr("") });
            if (kind == "a") placed++;
            else if (kind == "m") edited++;
            else if (kind == "d") deleted++;
            count++;
        }
        this->createObjects(editor, toCreate);
        this->applyColors(editor, colorEntries);
        if (count == 0) return 0;

        for (auto& [uuid, str] : toCreate) {
            if (auto it = m_byUuid.find(uuid); it != m_byUuid.end()) rects.push_back(it->second->getObjectRect());
        }
        auto color = colorForClient(client);
        if (!rects.empty()) {
            if (rects.size() > 40) {
                // lots of objects: one box around all of them
                float x0 = rects[0].getMinX(), y0 = rects[0].getMinY(), x1 = rects[0].getMaxX(), y1 = rects[0].getMaxY();
                for (auto& r : rects) {
                    x0 = std::min(x0, r.getMinX()); y0 = std::min(y0, r.getMinY());
                    x1 = std::max(x1, r.getMaxX()); y1 = std::max(y1, r.getMaxY());
                }
                rects = { CCRect(x0, y0, x1 - x0, y1 - y0) };
            }
            highlights.push_back({ std::move(rects), author, color });
            if (highlights.size() > 20) highlights.erase(highlights.begin());
        }
        std::string what;
        auto add = [&](int n, char const* verb) {
            if (n > 0) what += fmt::format("{}{} {}", what.empty() ? "" : ", ", verb, n);
        };
        add(placed, "placed");
        add(edited, "edited");
        add(deleted, "deleted");
        if (!colorEntries.empty()) {
            what += fmt::format("{}changed {} color{}", what.empty() ? "" : ", ", colorEntries.size(), colorEntries.size() == 1 ? "" : "s");
        }
        feed.push_back({ fmt::format("{} {}", author, what), color, 0.f });
        while (feed.size() > 4) feed.pop_front();
        return count;
    }

    // Sets color channels from their saved form (same format GD saves levels in)
    void applyColors(LevelEditorLayer* editor, std::vector<std::string> const& entries) {
        if (entries.empty() || !editor->m_levelSettings || !editor->m_levelSettings->m_effectManager) return;
        auto settingsColors = editor->m_levelSettings->m_effectManager;
        for (auto& entry : entries) {
            int id = colorEntryChannel(entry);
            if (id <= 0) continue;
            auto action = settingsColors->getColorAction(id);
            if (!action) continue;
            action->setupFromString(entry);
            action->m_color = action->m_fromColor;
            action->m_currentOpacity = action->m_fromOpacity;
        }
        // copy the level's colors into the live editor so they show right away
        if (editor->m_effectManager) {
            editor->transferDefaultColors(settingsColors, editor->m_effectManager);
            editor->m_effectManager->calculateBaseActiveColors();
            editor->m_effectManager->processInheritedColors();
        }
        // remember how GD saves them now, so we don't send them straight back
        auto now = splitColors(levelColorString(editor));
        for (auto& entry : entries) {
            int id = colorEntryChannel(entry);
            if (auto it = now.find(id); it != now.end()) m_colors[id] = it->second;
        }
    }

    void removeTracked(LevelEditorLayer* editor, GameObject* obj) {
        Ref<GameObject> hold = obj; // stay valid while we remove it
        if (auto it = m_tracked.find(obj); it != m_tracked.end()) {
            m_byUuid.erase(it->second.uuid);
            m_tracked.erase(it);
        }
        // the player may have deleted it already. (Don't check getParent(): GD unloads objects
        // that are off screen, and skipping those left old copies behind, like "h", "he", "hel"...)
        if (!editor->m_objects || !editor->m_objects->containsObject(obj)) return;
        if (obj->m_isSelected && editor->m_editorUI) editor->m_editorUI->deselectObject(obj);
        editor->removeObject(obj, true);
    }

    void track(LevelEditorLayer* editor, GameObject* obj, std::string const& uuid) {
        m_tracked[obj] = { uuid, saveString(editor, obj), obj };
        m_byUuid[uuid] = obj;
    }

    void createObjects(LevelEditorLayer* editor, std::vector<std::pair<std::string, std::string>> const& objects) {
        std::vector<std::pair<std::string, std::string>> list;
        for (auto& o : objects) if (!o.second.empty()) list.push_back(o);
        if (list.empty()) return;

        // Fast path: create everything in one go (big levels would freeze the game otherwise)
        if (list.size() > 1) {
            std::string all;
            for (auto& [uuid, str] : list) {
                all += str;
                if (!str.empty() && str.back() != ';') all += ';';
            }
            auto created = editor->createObjectsFromString(all, true, true);
            if (created && created->count() == list.size()) {
                for (size_t i = 0; i < list.size(); i++) {
                    this->track(editor, static_cast<GameObject*>(created->objectAtIndex(i)), list[i].first);
                }
                return;
            }
            // counts didn't line up: undo and fall back to one by one
            log::warn("[collab] Batch create made {} of {} objects, creating one by one", created ? created->count() : 0, list.size());
            if (created) {
                for (auto obj : CCArrayExt<GameObject*>(created)) editor->removeObject(obj, true);
            }
        }
        for (auto& [uuid, str] : list) {
            auto created = editor->createObjectsFromString(str, true, true);
            if (!created || created->count() == 0) continue;
            this->track(editor, static_cast<GameObject*>(created->objectAtIndex(0)), uuid);
        }
    }

    void clearLevel(LevelEditorLayer* editor) {
        m_tracked.clear();
        m_byUuid.clear();
        // GD's own "delete everything": fast, and also clears undo/redo so nothing points at deleted objects
        editor->removeAllObjects();
    }

    void ping() {
        m_pinging = true;
        auto body = matjson::makeObject({
            { "room", room }, { "client", m_client }, { "name", playerName() }, { "last_seen", isoNow() },
        });
        if (m_cursorColumns) {
            body.set("cursor_x", std::round(cursor.x * 10.f) / 10.f);
            body.set("cursor_y", std::round(cursor.y * 10.f) / 10.f);
            body.set("has_cursor", hasCursor);
        }
        auto req = supa();
        req.header("Prefer", "resolution=merge-duplicates,return=minimal");
        req.bodyJSON(body);
        auto roomCode = room;
        m_pingTask.spawn(req.post(fmt::format("{}/rest/v1/collab_members?on_conflict=room,client", SUPA_URL)), [this, roomCode](web::WebResponse res) {
            if (!active || room != roomCode) { m_pinging = false; return; }
            if (!res.ok() && m_cursorColumns && res.string().unwrapOr("").find("cursor") != std::string::npos) {
                // the cursor columns don't exist: keep working without cursors
                log::warn("[collab] Database has no cursor columns yet, run sql/update-cursors.sql");
                m_cursorColumns = false;
                m_pinging = false;
                return;
            }
            auto url = fmt::format(
                "{}/rest/v1/collab_members?room=eq.{}&last_seen=gt.{}&select={}&order=name.asc",
                SUPA_URL, roomCode, isoNow(-20), m_cursorColumns ? "client,name,cursor_x,cursor_y,has_cursor" : "client,name"
            );
            m_pingTask.spawn(supa().get(url), [this, roomCode](web::WebResponse res) {
                m_pinging = false;
                if (!active || room != roomCode || !res.ok()) return;
                auto json = res.json().unwrapOr(matjson::Value());
                if (!json.isArray()) return;
                members.clear();
                others.clear();
                for (auto& m : json) {
                    auto name = m["name"].asString().unwrapOr("?");
                    members.push_back(name);
                    auto client = m["client"].asString().unwrapOr("");
                    if (client.empty() || client == m_client) continue;
                    Member member;
                    member.client = client;
                    member.name = name;
                    member.x = static_cast<float>(m["cursor_x"].asDouble().unwrapOr(0.0));
                    member.y = static_cast<float>(m["cursor_y"].asDouble().unwrapOr(0.0));
                    member.hasCursor = m["has_cursor"].asBool().unwrapOr(false);
                    member.color = colorForClient(client);
                    others.push_back(std::move(member));
                }
            });
        });
    }

    void saveSnapshot(LevelEditorLayer* editor) {
        // make sure everything we have is already sent and tracked
        auto ops = this->scanLocal(editor, false);
        if (ops.size() > 0) return this->push(std::move(ops)); // try again next time
        auto body = matjson::makeObject({
            { "snapshot", this->snapshotJson() },
            { "snapshot_event", std::max(m_lastEvent, m_maxOwnEvent) },
            { "updated_at", isoNow() },
        });
        auto req = supa();
        req.header("Prefer", "return=minimal");
        req.bodyJSON(body);
        m_snapTask.spawn(req.patch(fmt::format("{}/rest/v1/collab_rooms?code=eq.{}", SUPA_URL, room)), [](web::WebResponse res) {
            if (!res.ok()) log::warn("[collab] Couldn't save room copy: HTTP {}", res.code());
        });
    }
};

// ---------------------------------------------------------------------------
// Editor hooks: a small node that ticks the session and shows the status
// ---------------------------------------------------------------------------

// Set by the My Levels Collab button: what to do once the new level's editor opens
enum class PendingAction { None, Host, Join };
static PendingAction s_pendingAction = PendingAction::None;
static std::string s_pendingCode;

// Last spot touched in the editor (phones have no mouse)
static CCPoint s_lastTouch = { 0.f, 0.f };
static bool s_touchValid = false;

class CollabTicker : public CCNode {
public:
    static constexpr float FLASH_TIME = 2.5f;
    static constexpr float FEED_TIME = 12.f;

    WeakRef<LevelEditorLayer> m_editor;
    CCLabelBMFont* m_label = nullptr;
    CCLabelBMFont* m_feedLabels[4] = {};
    float m_elapsed = 0.f;
    float m_startDelay = 0.f;

    // cursors and flashes live inside the editor's object layer, so they move and zoom with the level
    Ref<CCNode> m_overlay;
    CCDrawNode* m_flashDraw = nullptr;

    struct CursorNode {
        Ref<CCNode> node;
        CCPoint pos;
        CCPoint target;
    };
    std::unordered_map<std::string, CursorNode> m_cursors;

    struct Flash {
        std::vector<CCRect> rects;
        ccColor3B color;
        float life = FLASH_TIME;
        Ref<CCLabelBMFont> tag;
    };
    std::vector<Flash> m_flashes;

    static CollabTicker* create(LevelEditorLayer* editor) {
        auto ret = new CollabTicker();
        ret->m_editor = editor;
        ret->init();
        ret->autorelease();
        return ret;
    }

    bool init() override {
        if (!CCNode::init()) return false;
        auto winSize = CCDirector::sharedDirector()->getWinSize();
        m_label = CCLabelBMFont::create("", "chatFont.fnt");
        m_label->setAnchorPoint({ 0.f, 1.f });
        m_label->setScale(0.55f);
        m_label->setPosition({ 8.f, winSize.height - 60.f });
        m_label->setOpacity(220);
        this->addChild(m_label);
        for (int i = 0; i < 4; i++) {
            auto lbl = CCLabelBMFont::create("", "chatFont.fnt");
            lbl->setAnchorPoint({ 0.f, 1.f });
            lbl->setScale(0.5f);
            lbl->setPosition({ 8.f, winSize.height - 74.f - i * 12.f });
            this->addChild(lbl);
            m_feedLabels[i] = lbl;
        }
        s_touchValid = false;
        this->scheduleUpdate();
        return true;
    }

    void runPendingAction(LevelEditorLayer* editor) {
        auto action = s_pendingAction;
        auto code = s_pendingCode;
        s_pendingAction = PendingAction::None;
        s_pendingCode.clear();
        if (action == PendingAction::Join) {
            Notification::create(fmt::format("Joining room {}...", code), NotificationIcon::Loading, 1.f)->show();
            CollabSession::get().join(editor, code, [](bool ok, std::string err) {
                if (!ok) Notification::create(err, NotificationIcon::Error, 3.f)->show();
                else Notification::create("Joined! Edits now sync live", NotificationIcon::Success)->show();
            });
        } else if (action == PendingAction::Host) {
            Notification::create("Creating room...", NotificationIcon::Loading, 1.f)->show();
            CollabSession::get().host(editor, [](bool ok, std::string err) {
                if (!ok) Notification::create(err, NotificationIcon::Error, 3.f)->show();
                else Notification::create(fmt::format("Room {} created! Share the code", CollabSession::get().room), NotificationIcon::Success, 3.f)->show();
            });
        }
    }

    void update(float dt) override {
        auto editor = m_editor.lock();
        if (!editor) return;
        auto& s = CollabSession::get();

        if (s_pendingAction != PendingAction::None) {
            m_startDelay += dt;
            if (m_startDelay > 0.6f) this->runPendingAction(editor);
        }

        bool mine = s.isActiveFor(editor);
        m_label->setVisible(mine);
        if (!mine) {
            for (auto lbl : m_feedLabels) lbl->setVisible(false);
            if (m_overlay) m_overlay->setVisible(false);
            return;
        }

        this->updateLocalCursor(editor);

        m_elapsed += dt;
        float interval = static_cast<float>(Mod::get()->getSettingValue<double>("sync-interval"));
        if (m_elapsed >= interval) {
            m_elapsed = 0.f;
            s.tick(editor);
        }
        m_label->setString(fmt::format("Collab {}  |  {} online  |  {}", s.room, std::max<size_t>(1, s.members.size()), s.status).c_str());

        this->updateFeed(dt);
        if (this->ensureOverlay(editor)) {
            m_overlay->setVisible(true);
            float zoom = std::max(0.05f, editor->m_objectLayer->getScale());
            this->updateCursors(dt, zoom);
            this->updateFlashes(dt, zoom);
        }
    }

    void updateLocalCursor(LevelEditorLayer* editor) {
        auto& s = CollabSession::get();
        if (!editor->m_objectLayer) return;
#ifdef GEODE_IS_DESKTOP
        // PC: show where we last placed or moved a block
        if (!s.hasEditPos) return;
        s.cursor = s.lastEditPos;
        s.hasCursor = true;
#else
        if (!s_touchValid) return;
        s.cursor = editor->m_objectLayer->convertToNodeSpace(s_lastTouch);
        s.hasCursor = true;
#endif
    }

    void updateFeed(float dt) {
        auto& feed = CollabSession::get().feed;
        for (auto& line : feed) line.age += dt;
        while (!feed.empty() && feed.front().age > FEED_TIME) feed.pop_front();
        for (size_t i = 0; i < 4; i++) {
            auto lbl = m_feedLabels[i];
            if (i >= feed.size()) {
                lbl->setVisible(false);
                continue;
            }
            // newest line on top
            auto& line = feed[feed.size() - 1 - i];
            lbl->setVisible(true);
            lbl->setString(line.text.c_str());
            lbl->setColor(line.color);
            float left = FEED_TIME - line.age;
            lbl->setOpacity(static_cast<GLubyte>(230.f * std::clamp(left / 2.f, 0.f, 1.f)));
        }
    }

    bool ensureOverlay(LevelEditorLayer* editor) {
        auto layer = editor->m_objectLayer;
        if (!layer) return false;
        if (!m_overlay) {
            m_overlay = CCNode::create();
            m_overlay->setID("collab-overlay"_spr);
            m_flashDraw = CCDrawNode::create();
            m_overlay->addChild(m_flashDraw);
        }
        if (m_overlay->getParent() != layer) {
            m_overlay->removeFromParent();
            layer->addChild(m_overlay, 9999);
        }
        return true;
    }

    static CCNode* makeCursor(std::string const& name, ccColor3B color) {
        auto node = CCNode::create();
        auto draw = CCDrawNode::create();
        CCPoint arrow[] = { { 0.f, 0.f }, { 0.f, -20.f }, { 5.f, -15.f }, { 9.f, -23.f }, { 12.5f, -21.5f }, { 8.5f, -14.f }, { 15.f, -14.f } };
        draw->drawPolygon(arrow, 7, ccc4FFromccc3B(color), 1.2f, { 0.f, 0.f, 0.f, 1.f });
        node->addChild(draw);

        auto lbl = CCLabelBMFont::create(name.c_str(), "chatFont.fnt");
        lbl->setScale(0.5f);
        lbl->limitLabelWidth(110.f, 0.5f, 0.2f);
        lbl->setColor(color);
        lbl->setAnchorPoint({ 0.f, 0.5f });
        auto size = lbl->getScaledContentSize();
        auto bg = CCLayerColor::create({ 0, 0, 0, 170 }, size.width + 8.f, size.height + 2.f);
        bg->setPosition({ 12.f, -30.f - size.height / 2.f - 1.f });
        lbl->setPosition({ 16.f, -30.f });
        node->addChild(bg);
        node->addChild(lbl);
        return node;
    }

    void updateCursors(float dt, float zoom) {
        auto& s = CollabSession::get();
        std::unordered_set<std::string> seen;
        for (auto& m : s.others) {
            if (!m.hasCursor) continue;
            seen.insert(m.client);
            CCPoint target = { m.x, m.y };
            auto it = m_cursors.find(m.client);
            if (it == m_cursors.end()) {
                auto node = makeCursor(m.name, m.color);
                node->setPosition(target);
                m_overlay->addChild(node, 10);
                m_cursors[m.client] = { node, target, target };
            } else {
                it->second.target = target;
            }
        }
        for (auto it = m_cursors.begin(); it != m_cursors.end();) {
            if (!seen.contains(it->first)) {
                it->second.node->removeFromParent();
                it = m_cursors.erase(it);
                continue;
            }
            auto& c = it->second;
            // glide toward the latest position so cursors don't jump once a second
            float t = std::min(1.f, dt * 8.f);
            c.pos = c.pos + (c.target - c.pos) * t;
            c.node->setPosition(c.pos);
            c.node->setScale(1.f / zoom);
            ++it;
        }
    }

    void updateFlashes(float dt, float zoom) {
        auto& s = CollabSession::get();
        for (auto& h : s.highlights) {
            Flash f;
            f.rects = std::move(h.rects);
            f.color = h.color;
            float x0 = f.rects[0].getMinX(), y1 = f.rects[0].getMaxY();
            for (auto& r : f.rects) { x0 = std::min(x0, r.getMinX()); y1 = std::max(y1, r.getMaxY()); }
            auto tag = CCLabelBMFont::create(h.name.c_str(), "chatFont.fnt");
            tag->setColor(h.color);
            tag->setAnchorPoint({ 0.f, 0.f });
            tag->setPosition({ x0, y1 + 2.f });
            m_overlay->addChild(tag, 5);
            f.tag = tag;
            m_flashes.push_back(std::move(f));
        }
        s.highlights.clear();

        m_flashDraw->clear();
        for (auto it = m_flashes.begin(); it != m_flashes.end();) {
            it->life -= dt;
            if (it->life <= 0.f) {
                it->tag->removeFromParent();
                it = m_flashes.erase(it);
                continue;
            }
            float a = it->life / FLASH_TIME;
            auto c = ccc4FFromccc3B(it->color);
            ccColor4F fill = { c.r, c.g, c.b, 0.3f * a };
            ccColor4F line = { c.r, c.g, c.b, a };
            for (auto& r : it->rects) {
                CCPoint box[] = {
                    { r.getMinX(), r.getMinY() }, { r.getMaxX(), r.getMinY() },
                    { r.getMaxX(), r.getMaxY() }, { r.getMinX(), r.getMaxY() },
                };
                m_flashDraw->drawPolygon(box, 4, fill, 1.5f / zoom, line);
            }
            it->tag->setScale(0.5f / zoom);
            it->tag->setOpacity(static_cast<GLubyte>(255.f * a));
            ++it;
        }
    }

    void onExit() override {
        // leaving the editor (exit, save & play...) ends the session
        auto editor = m_editor.lock();
        if (editor && CollabSession::get().isActiveFor(editor)) CollabSession::get().leave();
        if (m_overlay) m_overlay->removeFromParent();
        m_cursors.clear();
        m_flashes.clear();
        CCNode::onExit();
    }
};

#ifndef GEODE_IS_DESKTOP
class $modify(CollabEditorUI, EditorUI) {
    bool ccTouchBegan(CCTouch* touch, CCEvent* event) {
        s_lastTouch = touch->getLocation();
        s_touchValid = true;
        return EditorUI::ccTouchBegan(touch, event);
    }
    void ccTouchMoved(CCTouch* touch, CCEvent* event) {
        s_lastTouch = touch->getLocation();
        EditorUI::ccTouchMoved(touch, event);
    }
};
#endif

class $modify(CollabEditorLayer, LevelEditorLayer) {
    bool init(GJGameLevel* level, bool noUI) {
        if (!LevelEditorLayer::init(level, noUI)) return false;
        auto ticker = CollabTicker::create(this);
        ticker->setID("collab-ticker"_spr);
        this->addChild(ticker, 1000);
        return true;
    }
};

// ---------------------------------------------------------------------------
// The Collab popup (opened from the editor pause menu)
// ---------------------------------------------------------------------------

class CollabPopup : public Popup {
protected:
    static constexpr float W = 340.f;
    static constexpr float H = 240.f;
    WeakRef<LevelEditorLayer> m_editor;
    CCNode* m_content = nullptr;
    TextInput* m_codeInput = nullptr;
    bool m_busy = false;
    float m_refresh = 0.f;

    bool init(LevelEditorLayer* editor) {
        if (!Popup::init(W, H)) return false;
        m_editor = editor;
        this->setTitle("Live Collab");
        this->rebuild();
        this->schedule(schedule_selector(CollabPopup::refresh), 1.f);
        return true;
    }

    void refresh(float) {
        if (CollabSession::get().active) this->rebuild();
    }

    CCMenuItemSpriteExtra* button(char const* text, char const* texture, float scale, std::function<void()> fn) {
        auto spr = ButtonSprite::create(text, "goldFont.fnt", texture, 0.8f);
        spr->setScale(scale);
        return CCMenuItemExt::createSpriteExtra(spr, [fn](CCMenuItemSpriteExtra*) { fn(); });
    }

    CCLabelBMFont* text(std::string const& str, char const* font, float scale, CCPoint pos, ccColor3B color = { 255, 255, 255 }) {
        auto lbl = CCLabelBMFont::create(str.c_str(), font);
        lbl->setScale(scale);
        lbl->setColor(color);
        lbl->setPosition(pos);
        m_content->addChild(lbl);
        return lbl;
    }

    void rebuild() {
        if (m_content) m_content->removeFromParent();
        m_content = CCNode::create();
        m_mainLayer->addChild(m_content);
        auto menu = CCMenu::create();
        menu->setPosition({ 0.f, 0.f });
        m_content->addChild(menu);
        auto& s = CollabSession::get();
        auto editor = m_editor.lock();

        if (!s.active) {
            this->text("Build this level together with friends, live.", "chatFont.fnt", 0.6f, { W / 2, H - 50.f });

            auto host = this->button("Host a room", "GJ_button_01.png", 0.75f, [this] { this->doHost(); });
            host->setPosition({ W / 2, H - 85.f });
            menu->addChild(host);

            this->text("or join with a code:", "chatFont.fnt", 0.55f, { W / 2, H - 120.f }, { 200, 200, 200 });
            m_codeInput = TextInput::create(140.f, "CODE", "bigFont.fnt");
            m_codeInput->setFilter("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789");
            m_codeInput->setMaxCharCount(8);
            m_codeInput->setPosition({ W / 2 - 40.f, H - 150.f });
            m_content->addChild(m_codeInput);
            auto join = this->button("Join", "GJ_button_02.png", 0.7f, [this] { this->doJoin(); });
            join->setPosition({ W / 2 + 85.f, H - 150.f });
            menu->addChild(join);

            this->text("Joining replaces the objects in this level with the room's level.", "chatFont.fnt", 0.45f, { W / 2, 40.f }, { 255, 190, 140 });
            this->text("Level settings (song, colors, start) aren't synced yet.", "chatFont.fnt", 0.45f, { W / 2, 26.f }, { 200, 200, 200 });
            return;
        }

        this->text(s.isHost ? "You're hosting room" : "You're in room", "chatFont.fnt", 0.6f, { W / 2, H - 48.f });
        this->text(s.room, "goldFont.fnt", 1.1f, { W / 2 - 30.f, H - 75.f });
        auto copy = this->button("Copy", "GJ_button_04.png", 0.5f, [code = s.room] {
            if (clipboard::write(code)) Notification::create("Room code copied", NotificationIcon::Success)->show();
        });
        copy->setPosition({ W / 2 + 60.f, H - 75.f });
        menu->addChild(copy);

        std::string who;
        for (size_t i = 0; i < s.members.size(); i++) who += (i ? ", " : "") + s.members[i];
        if (who.empty()) who = playerName();
        auto online = this->text(fmt::format("Online ({}): {}", std::max<size_t>(1, s.members.size()), who), "chatFont.fnt", 0.55f, { W / 2, H - 108.f });
        online->limitLabelWidth(W - 40.f, 0.55f, 0.2f);
        this->text(s.status, "chatFont.fnt", 0.5f, { W / 2, H - 128.f }, { 180, 220, 255 });

        this->text("Share the code. Friends press Collab in My Levels and type it.", "chatFont.fnt", 0.45f, { W / 2, 78.f }, { 200, 200, 200 });
        this->text("Everyone should save their own copy of the level.", "chatFont.fnt", 0.45f, { W / 2, 65.f }, { 200, 200, 200 });

        auto leave = this->button("Leave room", "GJ_button_06.png", 0.65f, [this] {
            CollabSession::get().leave();
            this->rebuild();
        });
        leave->setPosition({ W / 2, 32.f });
        menu->addChild(leave);
    }

    void doHost() {
        auto editor = m_editor.lock();
        if (!editor || m_busy) return;
        m_busy = true;
        Notification::create("Creating room...", NotificationIcon::Loading, 1.f)->show();
        auto self = WeakRef<CollabPopup>(this);
        CollabSession::get().host(editor, [self](bool ok, std::string err) {
            auto popup = self.lock();
            if (popup) {
                popup->m_busy = false;
                popup->rebuild();
            }
            if (!ok) Notification::create(err, NotificationIcon::Error, 3.f)->show();
            else Notification::create("Room created! Share the code", NotificationIcon::Success)->show();
        });
    }

    void doJoin() {
        auto editor = m_editor.lock();
        if (!editor || m_busy || !m_codeInput) return;
        std::string code = m_codeInput->getString();
        for (auto& c : code) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        if (code.size() < 4) {
            Notification::create("Enter the room code first", NotificationIcon::Warning)->show();
            return;
        }
        auto self = WeakRef<CollabPopup>(this);
        createQuickPopup(
            "Join room",
            fmt::format("Join room <cy>{}</c>? The objects in this level will be <cr>replaced</c> with the room's level.", code),
            "Cancel", "Join",
            [self, code](auto, bool yes) {
                auto popup = self.lock();
                if (!yes || !popup) return;
                auto editor = popup->m_editor.lock();
                if (!editor) return;
                popup->m_busy = true;
                Notification::create("Joining...", NotificationIcon::Loading, 1.f)->show();
                CollabSession::get().join(editor, code, [self](bool ok, std::string err) {
                    auto popup = self.lock();
                    if (popup) {
                        popup->m_busy = false;
                        popup->rebuild();
                    }
                    if (!ok) Notification::create(err, NotificationIcon::Error, 3.f)->show();
                    else Notification::create("Joined! Edits now sync live", NotificationIcon::Success)->show();
                });
            }
        );
    }

public:
    static CollabPopup* create(LevelEditorLayer* editor) {
        auto ret = new CollabPopup();
        if (ret->init(editor)) {
            ret->autorelease();
            return ret;
        }
        delete ret;
        return nullptr;
    }
};

class $modify(CollabPauseLayer, EditorPauseLayer) {
    void customSetup() {
        EditorPauseLayer::customSetup();
        auto winSize = CCDirector::sharedDirector()->getWinSize();
        auto spr = ButtonSprite::create("Collab", "goldFont.fnt", "GJ_button_01.png", 0.8f);
        spr->setScale(0.7f);
        auto editor = WeakRef<LevelEditorLayer>(m_editorLayer);
        auto btn = CCMenuItemExt::createSpriteExtra(spr, [editor](CCMenuItemSpriteExtra*) {
            if (auto ed = editor.lock()) {
                if (auto popup = CollabPopup::create(ed)) popup->show();
            }
        });
        auto menu = CCMenu::create();
        menu->setID("collab-menu"_spr);
        menu->setPosition({ 0.f, 0.f });
        // left of the Settings / Export column, so it doesn't cover GD's buttons
        btn->setPosition({ winSize.width - 112.f, winSize.height - 52.f });
        menu->addChild(btn);
        this->addChild(menu, 100);
    }
};

// ---------------------------------------------------------------------------
// My Levels: a Collab button that makes a new level and hosts / joins a room
// ---------------------------------------------------------------------------

static void openCollabLevel(PendingAction action, std::string const& code) {
    auto level = GameLevelManager::get()->createNewLevel();
    if (!level) {
        Notification::create("Couldn't create a level", NotificationIcon::Error)->show();
        return;
    }
    std::string name = action == PendingAction::Join ? fmt::format("Collab {}", code) : std::string("Collab Level");
    level->m_levelName = name;
    s_pendingAction = action;
    s_pendingCode = code;
    CCDirector::sharedDirector()->replaceScene(CCTransitionFade::create(0.5f, LevelEditorLayer::scene(level, false)));
}

class CollabStartPopup : public Popup {
protected:
    static constexpr float W = 320.f;
    static constexpr float H = 220.f;
    TextInput* m_codeInput = nullptr;

    CCMenuItemSpriteExtra* button(char const* text, char const* texture, float scale, std::function<void()> fn) {
        auto spr = ButtonSprite::create(text, "goldFont.fnt", texture, 0.8f);
        spr->setScale(scale);
        return CCMenuItemExt::createSpriteExtra(spr, [fn](CCMenuItemSpriteExtra*) { fn(); });
    }

    CCLabelBMFont* text(std::string const& str, float scale, CCPoint pos, ccColor3B color = { 255, 255, 255 }) {
        auto lbl = CCLabelBMFont::create(str.c_str(), "chatFont.fnt");
        lbl->setScale(scale);
        lbl->setColor(color);
        lbl->setPosition(pos);
        m_mainLayer->addChild(lbl);
        return lbl;
    }

    bool init() {
        if (!Popup::init(W, H)) return false;
        this->setTitle("Live Collab");
        auto menu = CCMenu::create();
        menu->setPosition({ 0.f, 0.f });
        m_mainLayer->addChild(menu);

        this->text("Join a friend's room with their code:", 0.6f, { W / 2, H - 52.f });
        m_codeInput = TextInput::create(140.f, "CODE", "bigFont.fnt");
        m_codeInput->setFilter("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789");
        m_codeInput->setMaxCharCount(8);
        m_codeInput->setPosition({ W / 2 - 40.f, H - 85.f });
        m_mainLayer->addChild(m_codeInput);
        auto join = this->button("Join", "GJ_button_02.png", 0.7f, [this] { this->doJoin(); });
        join->setPosition({ W / 2 + 85.f, H - 85.f });
        menu->addChild(join);

        this->text("or start your own:", 0.55f, { W / 2, H - 122.f }, { 200, 200, 200 });
        auto host = this->button("New collab level", "GJ_button_01.png", 0.7f, [this] {
            this->onClose(nullptr);
            openCollabLevel(PendingAction::Host, "");
        });
        host->setPosition({ W / 2, H - 150.f });
        menu->addChild(host);

        this->text("Both make a new level in My Levels and open the editor.", 0.45f, { W / 2, 26.f }, { 200, 200, 200 });
        return true;
    }

    void doJoin() {
        std::string code = m_codeInput->getString();
        for (auto& c : code) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        if (code.size() < 4) {
            Notification::create("Enter the room code first", NotificationIcon::Warning)->show();
            return;
        }
        this->onClose(nullptr);
        openCollabLevel(PendingAction::Join, code);
    }

public:
    static CollabStartPopup* create() {
        auto ret = new CollabStartPopup();
        if (ret->init()) {
            ret->autorelease();
            return ret;
        }
        delete ret;
        return nullptr;
    }
};

class $modify(CollabBrowserLayer, LevelBrowserLayer) {
    bool init(GJSearchObject* search) {
        if (!LevelBrowserLayer::init(search)) return false;
        if (!search || search->m_searchType != SearchType::MyLevels) return true;

        auto winSize = CCDirector::sharedDirector()->getWinSize();
        CCNode* icon = CCSprite::createWithSpriteFrameName("accountBtn_friends_001.png");
        if (icon) {
            icon->setScale(0.75f);
        } else {
            auto lbl = CCLabelBMFont::create("Co", "bigFont.fnt");
            lbl->setScale(0.6f);
            icon = lbl;
        }
        auto spr = CircleButtonSprite::create(icon, CircleBaseColor::Green, CircleBaseSize::Small);
        auto btn = CCMenuItemExt::createSpriteExtra(spr, [](CCMenuItemSpriteExtra*) {
            if (auto popup = CollabStartPopup::create()) popup->show();
        });
        btn->setID("collab-button"_spr);

        auto lbl = CCLabelBMFont::create("Collab", "bigFont.fnt");
        lbl->setScale(0.3f);

        auto menu = CCMenu::create();
        menu->setID("collab-menu"_spr);
        menu->setPosition({ 0.f, 0.f });
        // left side, above the page arrow
        CCPoint pos = { 28.f, winSize.height / 2.f + 48.f };
        btn->setPosition(pos);
        lbl->setPosition(pos - CCPoint(0.f, 24.f));
        menu->addChild(btn);
        this->addChild(menu, 10);
        this->addChild(lbl, 10);
        return true;
    }
};

// ---------------------------------------------------------------------------
// Self-updater (same as the other mods)
// ---------------------------------------------------------------------------

#if GEODE_COMP_GD_VERSION >= 22091
static constexpr auto RELEASE_BASE = "https://github.com/pester33/GD-Collab/releases/download/latest-build-2.2091/";
#else
static constexpr auto RELEASE_BASE = "https://github.com/pester33/GD-Collab/releases/download/latest-build/";
#endif

static async::TaskHolder<web::WebResponse> s_updateTask;
static bool s_updateBusy = false;

static void whenScreenReady(std::function<void()> fn, int tries = 0) {
    auto scene = CCDirector::sharedDirector()->getRunningScene();
    bool ready = scene && !typeinfo_cast<CCTransitionScene*>(scene);
    if (ready || tries > 600) {
        fn();
        return;
    }
    Loader::get()->queueInMainThread([fn = std::move(fn), tries]() mutable {
        whenScreenReady(std::move(fn), tries + 1);
    });
}

static void installUpdate(std::string version) {
    if (s_updateBusy) return;
    s_updateBusy = true;
    auto url = fmt::format("{}{}.geode", RELEASE_BASE, Mod::get()->getID());
    auto loading = Notification::create("Downloading GD Gaming Collab update...", NotificationIcon::Loading, 0.f);
    loading->show();
    web::WebRequest req;
    setTimeout(req, 90);
    s_updateTask.spawn(req.get(url), [loading = Ref(loading), version](web::WebResponse res) {
        s_updateBusy = false;
        loading->hide();
        auto const& data = res.data();
        if (!res.ok() || data.size() < 4 || data[0] != 'P' || data[1] != 'K') {
            Notification::create(fmt::format("Update failed to download (HTTP {})", res.code()), NotificationIcon::Error, 3.f)->show();
            return;
        }
        if (!file::writeBinarySafe(Mod::get()->getPackagePath(), data)) {
            Notification::create("Update failed to install", NotificationIcon::Error, 3.f)->show();
            return;
        }
        whenScreenReady([version] {
            createQuickPopup("Update installed", fmt::format("GD Gaming Collab <cg>{}</c> is installed.\nRestart GD to use it.", version),
                "Later", "Restart", [](auto, bool restart) { if (restart) game::restart(true); });
        });
    });
}

static void checkForUpdate(bool isRetry = false) {
    if (s_updateBusy) return;
    s_updateBusy = true;
    web::WebRequest req;
    setTimeout(req, 20);
    s_updateTask.spawn(req.get(fmt::format("{}version.txt", RELEASE_BASE)), [isRetry](web::WebResponse res) {
        s_updateBusy = false;
        auto body = utils::string::trim(res.string().unwrapOr(""));
        log::info("[updater] version.txt: HTTP {}, body '{}'", res.code(), body.substr(0, 40));
        if (!res.ok()) {
            if (!isRetry) Loader::get()->queueInMainThread([] { checkForUpdate(true); });
            return;
        }
        auto latest = VersionInfo::parse(body);
        if (!latest) return;
        auto current = Mod::get()->getVersion();
        if (!(latest.unwrap() > current)) return;
        auto version = latest.unwrap().toVString();
        whenScreenReady([version, current] {
            createQuickPopup("GD Gaming Collab",
                fmt::format("A new version is out: <cg>{}</c> (you have {}).\nUpdate now?", version, current.toVString()),
                "Later", "Update", [version](auto, bool update) { if (update) installUpdate(version); });
        });
    });
}

class $modify(CollabMenuLayer, MenuLayer) {
    bool init() {
        if (!MenuLayer::init()) return false;
        static bool firstTime = true;
        if (firstTime) {
            firstTime = false;
            if (Mod::get()->getSettingValue<bool>("check-updates")) checkForUpdate();
        }
        return true;
    }
};
