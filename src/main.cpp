#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/MenuLayer.hpp>
#include <Geode/modify/LevelSearchLayer.hpp>
#include <Geode/binding/GJGameLevel.hpp>
#include <Geode/binding/GameStatsManager.hpp>
#include <Geode/binding/ButtonSprite.hpp>
#include <Geode/binding/CCMenuItemSpriteExtra.hpp>
#include <Geode/binding/GJSearchObject.hpp>
#include <Geode/binding/LevelBrowserLayer.hpp>
#include <Geode/ui/TextInput.hpp>
#include <Geode/ui/Popup.hpp>
#include <Geode/ui/Notification.hpp>
#include <Geode/utils/web.hpp>
#include <Geode/utils/cocos.hpp>

#include <thread>
#include <unordered_set>

using namespace geode::prelude;

// everything here is only touched on the main thread (hooks + queueInMainThread)
// so no mutex needed
static std::string g_token;
static std::string g_username;
static std::unordered_set<int> g_pending; // beaten levels we havent sent yet
static std::unordered_set<int> g_handled; // stuff the server already knows about
static bool g_syncing = false;
static bool g_enumerated = false;

constexpr int CHUNK = 50;

static std::string apiBase() {
    auto b = Mod::get()->getSettingValue<std::string>("api-base");
    while (!b.empty() && b.back() == '/') b.pop_back();
    return b;
}
static bool autoSync() { return Mod::get()->getSettingValue<bool>("auto-sync"); }
static bool loggedIn() { return !g_token.empty(); }

static void saveSets() {
    std::vector<matjson::Value> p(g_pending.begin(), g_pending.end());
    std::vector<matjson::Value> h(g_handled.begin(), g_handled.end());
    Mod::get()->setSavedValue<matjson::Value>("pending", p);
    Mod::get()->setSavedValue<matjson::Value>("handled", h);
}

static void loadState() {
    g_token = Mod::get()->getSavedValue<std::string>("token", "");
    g_username = Mod::get()->getSavedValue<std::string>("username", "");
    auto p = Mod::get()->getSavedValue<matjson::Value>("pending", matjson::Value::array());
    if (p.isArray()) for (auto& v : p) { int id = (int)v.asInt().unwrapOr(0); if (id > 0) g_pending.insert(id); }
    auto h = Mod::get()->getSavedValue<matjson::Value>("handled", matjson::Value::array());
    if (h.isArray()) for (auto& v : h) { int id = (int)v.asInt().unwrapOr(0); if (id > 0) g_handled.insert(id); }
}

static void setToken(std::string const& t, std::string const& u) {
    g_token = t; g_username = u;
    Mod::get()->setSavedValue<std::string>("token", t);
    Mod::get()->setSavedValue<std::string>("username", u);
}
static void clearToken() {
    g_token = "";
    Mod::get()->setSavedValue<std::string>("token", std::string(""));
}

static bool enqueue(int id) {
    if (id <= 0 || g_handled.count(id) || g_pending.count(id)) return false;
    g_pending.insert(id);
    return true;
}

static void syncNow();

// grab everything the player already beat from the save file.
// completed online levels are keyed "c_<id>" in m_completedLevels
static void enumerateHistorical() {
    auto gsm = GameStatsManager::get();
    if (!gsm || !gsm->m_completedLevels) return;
    int added = 0;
    for (auto [key, value] : CCDictionaryExt<std::string, cocos2d::CCObject*>(gsm->m_completedLevels)) {
        if (key.rfind("c_", 0) != 0) continue;
        std::string idStr = key.substr(2);
        if (idStr.empty() || idStr.find_first_not_of("0123456789") != std::string::npos) continue;
        int id = std::atoi(idStr.c_str());
        if (id > 0 && gsm->hasCompletedOnlineLevel(id) && enqueue(id)) added++;
    }
    if (added) { saveSets(); log::info("queued {} past completions", added); }
}

static void enumerateAndSync() {
    if (!g_enumerated) { g_enumerated = true; enumerateHistorical(); }
    syncNow();
}

static void syncNow() {
    if (g_syncing || !loggedIn() || g_pending.empty()) return;

    std::vector<int> chunk;
    for (int id : g_pending) { chunk.push_back(id); if ((int)chunk.size() >= CHUNK) break; }
    if (chunk.empty()) return;
    g_syncing = true;

    std::vector<matjson::Value> arr;
    for (int id : chunk) arr.push_back(matjson::makeObject({ {"id", id} }));
    matjson::Value body = matjson::makeObject({ {"completions", arr} });

    std::string token = g_token;
    std::string url = apiBase() + "/levels/bulk-complete";

    std::thread([body, token, url]() {
        auto res = web::WebRequest()
            .certVerification(false)
            .header("Content-Type", "application/json")
            .header("Authorization", "Bearer " + token)
            .bodyJSON(body)
            .timeout(std::chrono::seconds(60))
            .postSync(url);
        int code = res.code();
        matjson::Value j = res.json().unwrapOr(matjson::Value());

        Loader::get()->queueInMainThread([code, j]() {
            g_syncing = false;
            if (code == 401) {
                clearToken();
                Notification::create("List Sync: please log in again", NotificationIcon::Warning)->show();
                return;
            }
            if (code != 200) {
                log::warn("sync failed (http {})", code);
                return; // stays pending, retried later
            }

            size_t before = g_pending.size();
            auto resolve = [&](const char* field) {
                if (!j.contains(field) || !j[field].isArray()) return;
                for (auto& v : j[field]) {
                    int id = v.isObject() ? (int)v["id"].asInt().unwrapOr(0) : (int)v.asInt().unwrapOr(0);
                    if (id <= 0) continue;
                    g_pending.erase(id);
                    g_handled.insert(id);
                }
            };
            resolve("marked");
            resolve("skipped");
            // dont retry stuff that failed for a real reason, only rate limits
            if (j.contains("errors") && j["errors"].isArray()) {
                for (auto& v : j["errors"]) {
                    int id = v.isObject() ? (int)v["id"].asInt().unwrapOr(0) : (int)v.asInt().unwrapOr(0);
                    std::string reason = (v.isObject() && v.contains("error")) ? v["error"].asString().unwrapOr("") : "";
                    if (id <= 0 || reason == "rate-limited") continue;
                    g_pending.erase(id);
                    g_handled.insert(id);
                }
            }
            saveSets();
            log::info("{} pending after sync", (int)g_pending.size());

            if (!g_pending.empty() && g_pending.size() < before) {
                Loader::get()->queueInMainThread([]() { syncNow(); });
            }
        });
    }).detach();
}

// self updater - checks the repo for a newer release and drops the .geode
// into the mods folder, geode picks it up next launch
static void checkForUpdates() {
    std::thread([]() {
        auto res = web::WebRequest()
            .certVerification(false)
            .header("User-Agent", "list-sync")
            .timeout(std::chrono::seconds(30))
            .getSync("https://api.github.com/repos/johnonenintingle/list-sync/releases/latest");
        if (res.code() != 200) return;
        auto j = res.json().unwrapOr(matjson::Value());
        if (!j.contains("tag_name")) return;

        auto tag = j["tag_name"].asString().unwrapOr("");
        auto ver = VersionInfo::parse(tag);
        if (!ver.isOk() || ver.unwrap() <= Mod::get()->getVersion()) return;

        std::string url;
        if (j.contains("assets") && j["assets"].isArray()) {
            for (auto& a : j["assets"]) {
                auto name = a["name"].asString().unwrapOr("");
                if (name.ends_with(".geode")) {
                    url = a["browser_download_url"].asString().unwrapOr("");
                    break;
                }
            }
        }
        if (url.empty()) return;

        auto dl = web::WebRequest()
            .certVerification(false)
            .header("User-Agent", "list-sync")
            .timeout(std::chrono::seconds(120))
            .getSync(url);
        if (dl.code() != 200) return;
        auto data = dl.data();
        // .geode files are zips, dont save an error page over the mod
        if (data.size() < 2 || data[0] != 'P' || data[1] != 'K') return;

        auto path = dirs::getModsDir() / (Mod::get()->getID() + ".geode");
        if (!file::writeBinary(path, data).isOk()) {
            log::warn("couldnt write update to mods folder");
            return;
        }
        Loader::get()->queueInMainThread([tag]() {
            Notification::create(
                fmt::format("List Sync {} downloaded, restart to apply", tag),
                NotificationIcon::Success
            )->show();
        });
    }).detach();
}

// pulls your recommendation queue from the site (personalized when logged in,
// levels youve already ranked are left out server side) and opens it as a
// level list. type 10 search takes a comma separated id string, same thing
// map packs use, so the browser does all the work
static void openRecommended() {
    if (!loggedIn()) {
        Notification::create("List Sync: log in first (Sync button on the main menu)", NotificationIcon::Warning)->show();
        return;
    }
    Notification::create("Loading recommendations...", NotificationIcon::Loading)->show();
    std::string url = apiBase() + "/lists/suggestions?limit=50";
    std::string token = g_token;

    std::thread([url, token]() {
        auto res = web::WebRequest()
            .certVerification(false)
            .header("Authorization", "Bearer " + token)
            .timeout(std::chrono::seconds(20))
            .getSync(url);
        int code = res.code();
        auto j = res.json().unwrapOr(matjson::Value());

        Loader::get()->queueInMainThread([code, j]() {
            if (code != 200 || !j.contains("levels") || !j["levels"].isArray()) {
                Notification::create("List Sync: couldn't load recommendations", NotificationIcon::Error)->show();
                return;
            }
            std::string ids;
            int count = 0;
            for (auto& l : j["levels"]) {
                long long id = l["levelId"].asInt().unwrapOr(0);
                if (id <= 0) continue;
                if (!ids.empty()) ids += ",";
                ids += std::to_string(id);
                if (++count >= 50) break;
            }
            if (ids.empty()) {
                Notification::create("List Sync: nothing to recommend right now", NotificationIcon::Warning)->show();
                return;
            }
            auto search = GJSearchObject::create(SearchType::MapPackOnClick, ids);
            auto scene = CCScene::create();
            scene->addChild(LevelBrowserLayer::create(search));
            CCDirector::sharedDirector()->pushScene(CCTransitionFade::create(0.5f, scene));
        });
    }).detach();
}

class ListSyncPopup : public geode::Popup {
protected:
    TextInput* m_user = nullptr;
    TextInput* m_pass = nullptr;

    bool init() {
        if (!Popup::init(300.f, 230.f)) return false;
        this->setTitle("List Sync");
        auto size = m_mainLayer->getContentSize();
        float cx = size.width / 2;

        if (!loggedIn()) {
            auto info = CCLabelBMFont::create("Link your Dynamic List account", "bigFont.fnt");
            info->setScale(0.45f);
            info->setPosition({ cx, size.height - 38 });
            m_mainLayer->addChild(info);

            m_user = TextInput::create(220, "Username", "bigFont.fnt");
            m_user->setCommonFilter(CommonFilter::Any);
            if (!g_username.empty()) m_user->setString(g_username);
            m_user->setPosition({ cx, size.height - 78 });
            m_mainLayer->addChild(m_user);

            m_pass = TextInput::create(220, "Password", "bigFont.fnt");
            m_pass->setCommonFilter(CommonFilter::Any);
            m_pass->setPasswordMode(true);
            m_pass->setPosition({ cx, size.height - 114 });
            m_mainLayer->addChild(m_pass);

            auto menu = CCMenu::create();
            menu->setPosition({ 0, 0 });
            auto btn = CCMenuItemSpriteExtra::create(
                ButtonSprite::create("Log in"), this, menu_selector(ListSyncPopup::onLogin));
            btn->setPosition({ cx, size.height - 158 });
            menu->addChild(btn);
            m_mainLayer->addChild(menu);
        } else {
            auto info = CCLabelBMFont::create(("Logged in as " + g_username).c_str(), "bigFont.fnt");
            info->setScale(0.5f);
            info->setPosition({ cx, size.height - 44 });
            m_mainLayer->addChild(info);

            auto cnt = CCLabelBMFont::create(
                (std::to_string(g_pending.size()) + " completion(s) pending").c_str(), "goldFont.fnt");
            cnt->setScale(0.6f);
            cnt->setPosition({ cx, size.height - 78 });
            m_mainLayer->addChild(cnt);

            auto menu = CCMenu::create();
            menu->setPosition({ 0, 0 });
            auto sync = CCMenuItemSpriteExtra::create(
                ButtonSprite::create("Sync now"), this, menu_selector(ListSyncPopup::onSync));
            sync->setPosition({ cx, size.height - 118 });
            menu->addChild(sync);
            auto out = CCMenuItemSpriteExtra::create(
                ButtonSprite::create("Log out"), this, menu_selector(ListSyncPopup::onLogout));
            out->setPosition({ cx, size.height - 158 });
            menu->addChild(out);
            m_mainLayer->addChild(menu);
        }
        return true;
    }

    void onLogin(CCObject*) {
        std::string u = m_user ? m_user->getString() : "";
        std::string p = m_pass ? m_pass->getString() : "";
        if (u.empty() || p.empty()) {
            Notification::create("Enter username and password", NotificationIcon::Error)->show();
            return;
        }
        Notification::create("Logging in...", NotificationIcon::Loading)->show();
        std::string url = apiBase() + "/auth/login";

        // popup might be gone by the time this finishes so dont touch `this` in here
        std::thread([u, p, url]() {
            auto body = matjson::makeObject({ {"username", u}, {"password", p} });
            auto res = web::WebRequest()
                .certVerification(false)
                .header("Content-Type", "application/json")
                .bodyJSON(body)
                .timeout(std::chrono::seconds(20))
                .postSync(url);
            int code = res.code();
            matjson::Value j = res.json().unwrapOr(matjson::Value());
            Loader::get()->queueInMainThread([code, j, u]() {
                if (code == 200 && j.contains("token")) {
                    std::string token = j["token"].asString().unwrapOr("");
                    std::string uname = u;
                    if (j.contains("user") && j["user"].isObject() && j["user"].contains("username"))
                        uname = j["user"]["username"].asString().unwrapOr(u);
                    if (!token.empty()) {
                        setToken(token, uname);
                        Notification::create("List Sync: logged in", NotificationIcon::Success)->show();
                        enumerateAndSync();
                        return;
                    }
                }
                std::string err = j.contains("error") ? j["error"].asString().unwrapOr("Login failed")
                                                      : ("HTTP " + std::to_string(code));
                Notification::create(("Login failed: " + err).c_str(), NotificationIcon::Error)->show();
            });
        }).detach();

        this->onClose(nullptr);
    }

    void onSync(CCObject*) {
        Notification::create("Syncing...", NotificationIcon::Loading)->show();
        enumerateAndSync();
        this->onClose(nullptr);
    }
    void onLogout(CCObject*) {
        clearToken();
        Notification::create("Logged out", NotificationIcon::Success)->show();
        this->onClose(nullptr);
    }

public:
    static ListSyncPopup* create() {
        auto ret = new ListSyncPopup();
        if (ret->init()) { ret->autorelease(); return ret; }
        CC_SAFE_DELETE(ret);
        return nullptr;
    }
};

class $modify(LSPlayLayer, PlayLayer) {
    void levelComplete() {
        PlayLayer::levelComplete();
        auto lvl = m_level;
        if (!lvl) return;
        if (m_isPracticeMode || m_isTestMode) return;
        int id = lvl->m_levelID.value();
        if (id <= 0) return;
        if (enqueue(id)) {
            saveSets();
            if (loggedIn() && autoSync()) syncNow();
        }
    }
};

class $modify(LSMenuLayer, MenuLayer) {
    bool init() {
        if (!MenuLayer::init()) return false;

        auto menu = CCMenu::create();
        menu->setID("list-sync-menu"_spr);
        auto spr = ButtonSprite::create("Sync");
        spr->setScale(0.55f);
        auto btn = CCMenuItemSpriteExtra::create(spr, this, menu_selector(LSMenuLayer::onListSync));
        menu->addChild(btn);
        auto win = CCDirector::sharedDirector()->getWinSize();
        menu->setPosition({ win.width - 36.f, win.height - 24.f });
        this->addChild(menu);

        // wait until the menu to sync so the save is definitely loaded
        if (loggedIn() && autoSync()) enumerateAndSync();

        static bool s_checked = false;
        if (!s_checked) {
            s_checked = true;
            checkForUpdates();
        }
        return true;
    }
    void onListSync(CCObject*) {
        if (auto p = ListSyncPopup::create()) p->show();
    }
};

class $modify(LSSearchLayer, LevelSearchLayer) {
    bool init(int type) {
        if (!LevelSearchLayer::init(type)) return false;

        auto spr = ButtonSprite::create("Recommended");
        spr->setScale(0.7f);
        auto btn = CCMenuItemSpriteExtra::create(spr, this, menu_selector(LSSearchLayer::onRecommended));
        btn->setID("recommended-button"_spr);

        // put it on the quick search tab, centered under the 2x4 grid, so it
        // shows/hides with the tab like the other buttons
        if (auto menu = this->getChildByID("quick-search-menu")) {
            float minY = FLT_MAX, maxY = -FLT_MAX, sumX = 0.f;
            int n = 0;
            for (auto child : CCArrayExt<CCNode*>(menu->getChildren())) {
                minY = std::min(minY, child->getPositionY());
                maxY = std::max(maxY, child->getPositionY());
                sumX += child->getPositionX();
                n++;
            }
            if (n > 0) {
                float rowGap = n > 2 ? (maxY - minY) / 3.f : 42.f;   // 4 rows
                btn->setPosition({ sumX / n, minY - rowGap });
                menu->addChild(btn);
                return true;
            }
        }
        // no ids? corner it is
        auto menu = CCMenu::create();
        menu->setID("list-sync-reco-menu"_spr);
        menu->addChild(btn);
        auto win = CCDirector::sharedDirector()->getWinSize();
        menu->setPosition({ win.width - 75.f, 22.f });
        this->addChild(menu);
        return true;
    }
    void onRecommended(CCObject*) { openRecommended(); }
};

$on_mod(Loaded) {
    loadState();
}
