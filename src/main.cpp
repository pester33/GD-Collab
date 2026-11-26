#include <Geode/Geode.hpp>
#include <Geode/modify/LevelEditorLayer.hpp>
#include <Geode/modify/EditorPauseLayer.hpp>
#include <Geode/modify/MenuLayer.hpp>
#include <Geode/ui/TextInput.hpp>
#include <Geode/utils/web.hpp>
#include <Geode/utils/async.hpp>
#include <Geode/utils/random.hpp>

#include <chrono>
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
            if (snapshot.isArray()) {
                for (auto& pair : snapshot) {
                    if (pair.isArray() && pair.size() == 2) {
                        objects.push_back({ pair[0].asString().unwrapOr(""), pair[1].asString().unwrapOr("") });
                    }
                }
            }
            this->createObjects(editor, objects);
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
            if (!ops.isNull()) this->push(std::move(ops));
        }
        // 2) pull in everyone else's edits (not while our own batch is on its way,
        //    so the newest change always wins the same way for everyone)
        if (!m_pushing && !m_polling) this->poll();
        // 3) who's here
        if (m_ticks % 4 == 1) this->ping();
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
    std::unordered_map<GameObject*, Tracked> m_tracked;
    std::unordered_map<std::string, GameObject*> m_byUuid;
    std::unordered_map<std::string, int64_t> m_ownLatest; // uuid -> id of our newest event touching it
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
        status = "Not in a room";
        m_editor = nullptr;
        m_tracked.clear();
        m_byUuid.clear();
        m_ownLatest.clear();
        m_lastEvent = 0;
        m_maxOwnEvent = 0;
        m_ticks = 0;
        m_pushing = false;
        m_polling = false;
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
                    if (!silent) ops.push_back(matjson::makeObject({ { "o", "a" }, { "u", uuid }, { "s", str } }));
                } else if (it->second.str != str) {
                    it->second.str = str;
                    if (!silent) ops.push_back(matjson::makeObject({ { "o", "m" }, { "u", it->second.uuid }, { "s", str } }));
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
        return ops.empty() ? matjson::Value() : matjson::Value(ops);
    }

    std::string snapshotJson() {
        std::vector<matjson::Value> list;
        list.reserve(m_tracked.size());
        for (auto& [obj, t] : m_tracked) list.push_back(matjson::Value(std::vector<matjson::Value>{ t.uuid, t.str }));
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
            if (!m_retry.isNull()) {
                auto retry = std::move(m_retry);
                m_retry = matjson::Value();
                this->push(std::move(retry));
            }
        });
    }
    matjson::Value m_retry;

    void poll() {
        m_polling = true;
        auto url = fmt::format(
            "{}/rest/v1/collab_events?room=eq.{}&id=gt.{}&client=neq.{}&select=id,author,data&order=id.asc&limit=200",
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
                if (ops.isArray()) applied += this->applyOps(editor, ops, id);
                m_lastEvent = std::max(m_lastEvent, id);
                lastAuthor = ev["author"].asString().unwrapOr("");
            }
            if (applied > 0) status = fmt::format("{} edited {} object{}", lastAuthor, applied, applied == 1 ? "" : "s");
        });
    }

    // Applies someone else's changes. Newest event wins: skip anything we changed more recently.
    int applyOps(LevelEditorLayer* editor, matjson::Value const& ops, int64_t eventId) {
        std::vector<std::pair<std::string, std::string>> toCreate;
        int count = 0;
        for (auto& op : ops) {
            auto kind = op["o"].asString().unwrapOr("");
            auto uuid = op["u"].asString().unwrapOr("");
            if (uuid.empty()) continue;
            auto own = m_ownLatest.find(uuid);
            if (own != m_ownLatest.end() && own->second > eventId) continue;

            if (auto existing = m_byUuid.find(uuid); existing != m_byUuid.end()) {
                this->removeTracked(editor, existing->second);
            }
            if (kind == "a" || kind == "m") toCreate.push_back({ uuid, op["s"].asString().unwrapOr("") });
            count++;
        }
        this->createObjects(editor, toCreate);
        return count;
    }

    void removeTracked(LevelEditorLayer* editor, GameObject* obj) {
        Ref<GameObject> hold = obj; // stay valid while we remove it
        if (auto it = m_tracked.find(obj); it != m_tracked.end()) {
            m_byUuid.erase(it->second.uuid);
            m_tracked.erase(it);
        }
        // the player may have deleted it already
        if (!obj->getParent()) return;
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
        auto body = matjson::makeObject({
            { "room", room }, { "client", m_client }, { "name", playerName() }, { "last_seen", isoNow() },
        });
        auto req = supa();
        req.header("Prefer", "resolution=merge-duplicates,return=minimal");
        req.bodyJSON(body);
        auto roomCode = room;
        m_pingTask.spawn(req.post(fmt::format("{}/rest/v1/collab_members?on_conflict=room,client", SUPA_URL)), [this, roomCode](web::WebResponse) {
            if (!active || room != roomCode) return;
            auto url = fmt::format("{}/rest/v1/collab_members?room=eq.{}&last_seen=gt.{}&select=name&order=name.asc", SUPA_URL, roomCode, isoNow(-25));
            m_pingTask.spawn(supa().get(url), [this, roomCode](web::WebResponse res) {
                if (!active || room != roomCode || !res.ok()) return;
                auto json = res.json().unwrapOr(matjson::Value());
                if (!json.isArray()) return;
                members.clear();
                for (auto& m : json) members.push_back(m["name"].asString().unwrapOr("?"));
            });
        });
    }

    void saveSnapshot(LevelEditorLayer* editor) {
        // make sure everything we have is already sent and tracked
        auto ops = this->scanLocal(editor, false);
        if (!ops.isNull()) return this->push(std::move(ops)); // try again next time
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

class CollabTicker : public CCNode {
public:
    WeakRef<LevelEditorLayer> m_editor;
    CCLabelBMFont* m_label = nullptr;
    float m_elapsed = 0.f;

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
        this->scheduleUpdate();
        return true;
    }

    void update(float dt) override {
        auto editor = m_editor.lock();
        if (!editor) return;
        auto& s = CollabSession::get();
        bool mine = s.isActiveFor(editor);
        m_label->setVisible(mine);
        if (!mine) return;

        m_elapsed += dt;
        float interval = static_cast<float>(Mod::get()->getSettingValue<double>("sync-interval"));
        if (m_elapsed >= interval) {
            m_elapsed = 0.f;
            s.tick(editor);
        }
        m_label->setString(fmt::format("Collab {}  |  {} online  |  {}", s.room, std::max<size_t>(1, s.members.size()), s.status).c_str());
    }

    void onExit() override {
        // leaving the editor (exit, save & play...) ends the session
        auto editor = m_editor.lock();
        if (editor && CollabSession::get().isActiveFor(editor)) CollabSession::get().leave();
        CCNode::onExit();
    }
};

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

        this->text("Share the code with friends. They open any level in the editor,", "chatFont.fnt", 0.45f, { W / 2, 78.f }, { 200, 200, 200 });
        this->text("pause, press Collab and join. Everyone should save their own copy.", "chatFont.fnt", 0.45f, { W / 2, 65.f }, { 200, 200, 200 });

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
    auto loading = Notification::create("Downloading GD Collab update...", NotificationIcon::Loading, 0.f);
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
            createQuickPopup("Update installed", fmt::format("GD Collab <cg>{}</c> is installed.\nRestart GD to use it.", version),
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
            createQuickPopup("GD Collab",
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
