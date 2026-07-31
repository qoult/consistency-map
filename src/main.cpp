#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PauseLayer.hpp>
#include <Geode/ui/GeodeUI.hpp>

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

using namespace geode::prelude;

namespace {
    // one section is one block wide: narrow enough to point at a single obstacle,
    // wide enough that the same jump always lands in the same bucket
    constexpr float SECTION_WIDTH = 30.f;
    // a spread this large counts as fully inconsistent when scoring a section
    constexpr double SPREAD_CEILING_TICKS = 4.0;

    struct Section {
        int reached = 0;        // attempts that played through this section
        int withClick = 0;      // attempts that pressed jump in it
        int n = 0;              // click samples behind mean/m2
        double mean = 0.0;      // Welford mean of the click x
        double m2 = 0.0;        // Welford sum of squared deviations
        double speedSum = 0.0;  // units per second, for converting x into ticks
        int speedN = 0;

        bool empty() const { return reached == 0 && n == 0; }

        double stddev() const { return n >= 2 ? std::sqrt(m2 / (n - 1)) : 0.0; }

        // the player covers this much x in one 240 TPS tick around here
        double unitsPerTick() const { return speedN > 0 ? (speedSum / speedN) / 240.0 : 0.0; }

        double spreadTicks() const {
            double units = unitsPerTick();
            return units > 0.0 ? stddev() / units : 0.0;
        }

        double clickRate() const { return reached > 0 ? static_cast<double>(withClick) / reached : 0.0; }
    };

    /*
    how shaky a section is, from 0 (rock solid) to 1, or -1 when there isn't
    enough data to say anything. two independent ways to be inconsistent:
    the click lands at a different place every time, or you press here on some
    attempts and not on others.
    */
    double riskOf(Section const& s, int minAttempts) {
        if (s.reached < minAttempts) return -1.0;

        double spread = std::clamp(s.spreadTicks() / SPREAD_CEILING_TICKS, 0.0, 1.0);
        // a coin flip is maximum indecision; always or never pressing is consistent
        double flip = 1.0 - std::abs(2.0 * s.clickRate() - 1.0);

        if (s.n < 2) return flip;
        return std::max(spread, flip);
    }

    // saved data is a plain "idx:reached:withClick:n:mean:m2:speedSum:speedN;..."
    // string per level, so no matjson serializers are needed
    std::string encodeSections(std::vector<Section> const& sections) {
        std::string out;
        for (size_t i = 0; i < sections.size(); i++) {
            auto const& s = sections[i];
            if (s.empty()) continue;
            if (!out.empty()) out.push_back(';');
            out += fmt::format(
                "{}:{}:{}:{}:{:.3f}:{:.3f}:{:.1f}:{}",
                i, s.reached, s.withClick, s.n, s.mean, s.m2, s.speedSum, s.speedN
            );
        }
        return out;
    }

    std::vector<Section> decodeSections(std::string const& raw) {
        std::vector<Section> out;
        size_t pos = 0;
        while (pos < raw.size()) {
            auto end = raw.find(';', pos);
            if (end == std::string::npos) end = raw.size();

            std::vector<std::string> parts;
            auto entry = raw.substr(pos, end - pos);
            size_t fieldPos = 0;
            while (fieldPos <= entry.size()) {
                auto colon = entry.find(':', fieldPos);
                if (colon == std::string::npos) {
                    parts.push_back(entry.substr(fieldPos));
                    break;
                }
                parts.push_back(entry.substr(fieldPos, colon - fieldPos));
                fieldPos = colon + 1;
            }

            if (parts.size() >= 8) {
                try {
                    size_t idx = static_cast<size_t>(std::stoul(parts[0]));
                    if (out.size() <= idx) out.resize(idx + 1);

                    Section s;
                    s.reached = std::stoi(parts[1]);
                    s.withClick = std::stoi(parts[2]);
                    s.n = std::stoi(parts[3]);
                    s.mean = std::stod(parts[4]);
                    s.m2 = std::stod(parts[5]);
                    s.speedSum = std::stod(parts[6]);
                    s.speedN = std::stoi(parts[7]);
                    out[idx] = s;
                }
                catch (...) {
                    // drop a corrupted entry instead of losing the whole level
                }
            }
            pos = end + 1;
        }
        return out;
    }

    std::string levelKey(GJGameLevel* level) {
        if (!level) return "cmap-unknown";
        auto id = level->m_levelID.value();
        if (id > 0) return "cmap-id-" + std::to_string(id);
        // local / unuploaded levels have no id, fall back to the name
        return "cmap-name-" + std::string(level->m_levelName);
    }

    size_t sectionOf(float x) {
        if (x <= 0.f) return 0;
        return static_cast<size_t>(x / SECTION_WIDTH);
    }

    ccColor3B colorForRisk(double risk) {
        double r = std::clamp(risk, 0.0, 1.0);
        auto red = static_cast<GLubyte>(std::lround(255.0 * std::min(1.0, r * 2.0)));
        auto green = static_cast<GLubyte>(std::lround(255.0 * std::min(1.0, (1.0 - r) * 2.0)));
        return ccc3(red, green, 60);
    }
}

class $modify(ConsistencyPlayLayer, PlayLayer) {
    struct Fields {
        std::vector<Section> m_sections;
        std::vector<char> m_clickedHere;  // per section, cleared every attempt
        std::string m_key;
        float m_startX = 0.f;
        float m_maxX = 0.f;
        float m_lastX = -1.f;
        bool m_committed = false;
        Ref<CCNode> m_strip = nullptr;
    };

    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        if (!PlayLayer::init(level, useReplay, dontCreateObjects)) return false;

        m_fields->m_key = levelKey(level);
        m_fields->m_sections = decodeSections(Mod::get()->getSavedValue<std::string>(m_fields->m_key));
        beginAttempt();
        rebuildStrip();
        return true;
    }

    bool tracking() {
        if (m_isPracticeMode && !Mod::get()->getSettingValue<bool>("track-practice")) return false;
        return !m_isTestMode;
    }

    Section& sectionAt(size_t idx) {
        if (m_fields->m_sections.size() <= idx) m_fields->m_sections.resize(idx + 1);
        return m_fields->m_sections[idx];
    }

    void beginAttempt() {
        m_fields->m_committed = false;
        m_fields->m_clickedHere.assign(m_fields->m_sections.size() + 1, 0);
        float x = m_player1 ? m_player1->getPositionX() : 0.f;
        m_fields->m_startX = x;
        m_fields->m_maxX = x;
        m_fields->m_lastX = -1.f;
    }

    // called from the GJBaseGameLayer hook whenever player 1 presses jump
    void recordClick() {
        if (!tracking() || !m_player1) return;

        float x = m_player1->getPositionX();
        if (x <= 0.f) return;

        size_t idx = sectionOf(x);
        auto& s = sectionAt(idx);

        // Welford, so the spread survives across sessions without keeping every click
        s.n += 1;
        double delta = x - s.mean;
        s.mean += delta / s.n;
        s.m2 += delta * (x - s.mean);

        if (m_fields->m_clickedHere.size() <= idx) m_fields->m_clickedHere.resize(idx + 1, 0);
        m_fields->m_clickedHere[idx] = 1;
    }

    void commitAttempt() {
        if (m_fields->m_committed || !tracking()) return;
        m_fields->m_committed = true;

        size_t from = sectionOf(m_fields->m_startX);
        size_t to = sectionOf(m_fields->m_maxX);
        for (size_t i = from; i <= to; i++) {
            auto& s = sectionAt(i);
            s.reached += 1;
            if (i < m_fields->m_clickedHere.size() && m_fields->m_clickedHere[i]) s.withClick += 1;
        }

        Mod::get()->setSavedValue<std::string>(m_fields->m_key, encodeSections(m_fields->m_sections));
    }

    void postUpdate(float dt) {
        PlayLayer::postUpdate(dt);
        if (!m_player1 || !tracking()) return;

        float x = m_player1->getPositionX();
        if (x > m_fields->m_maxX) m_fields->m_maxX = x;

        // sample how fast the player crosses this part of the level, so a spread in
        // x units can be reported in physics ticks
        if (m_fields->m_lastX >= 0.f && dt > 0.f) {
            double ups = (x - m_fields->m_lastX) / dt;
            if (ups > 1.0 && ups < 5000.0) {
                auto& s = sectionAt(sectionOf(x));
                s.speedSum += ups;
                s.speedN += 1;
            }
        }
        m_fields->m_lastX = x;
    }

    void destroyPlayer(PlayerObject* player, GameObject* object) {
        PlayLayer::destroyPlayer(player, object);
        if (player == m_player2) return;
        commitAttempt();
    }

    void levelComplete() {
        commitAttempt();
        PlayLayer::levelComplete();
    }

    void resetLevel() {
        PlayLayer::resetLevel();
        commitAttempt();  // covers quitting straight into a retry
        beginAttempt();
        rebuildStrip();
    }

    void onQuit() {
        commitAttempt();
        PlayLayer::onQuit();
    }

    /*
    the strip sits where the progress bar is and only paints the sections worth
    worrying about - drawing every section of a long level would mean a thousand
    nodes for a picture that is mostly green anyway
    */
    void rebuildStrip() {
        if (m_fields->m_strip) {
            m_fields->m_strip->removeFromParent();
            m_fields->m_strip = nullptr;
        }
        if (!Mod::get()->getSettingValue<bool>("show-strip")) return;
        if (m_levelLength <= 0.f) return;

        auto win = CCDirector::sharedDirector()->getWinSize();
        float barWidth = win.width * 0.6f;
        float left = (win.width - barWidth) / 2.f;
        float y = win.height - static_cast<float>(Mod::get()->getSettingValue<int64_t>("strip-offset"));
        float height = 5.f;

        auto strip = CCNode::create();
        strip->setZOrder(1000);
        strip->setPosition(ccp(left, y));

        auto back = CCLayerColor::create(ccc4(0, 0, 0, 90), barWidth, height);
        strip->addChild(back);

        int minAttempts = static_cast<int>(Mod::get()->getSettingValue<int64_t>("min-attempts"));
        double threshold = Mod::get()->getSettingValue<double>("risk-threshold");
        float unit = barWidth / m_levelLength;

        for (size_t i = 0; i < m_fields->m_sections.size(); i++) {
            double risk = riskOf(m_fields->m_sections[i], minAttempts);
            if (risk < threshold) continue;

            float w = std::max(2.f, SECTION_WIDTH * unit);
            auto color = colorForRisk(risk);
            auto mark = CCLayerColor::create(ccc4(color.r, color.g, color.b, 235), w, height);
            mark->setPositionX(std::min(barWidth - w, static_cast<float>(i) * SECTION_WIDTH * unit));
            strip->addChild(mark);
        }

        this->addChild(strip);
        m_fields->m_strip = strip;
    }
};

class $modify(ConsistencyGameLayer, GJBaseGameLayer) {
    void handleButton(bool down, int button, bool isPlayer1) {
        GJBaseGameLayer::handleButton(down, button, isPlayer1);

        if (!down || !isPlayer1 || button != static_cast<int>(PlayerButton::Jump)) return;
        if (auto pl = PlayLayer::get()) static_cast<ConsistencyPlayLayer*>(pl)->recordClick();
    }
};

namespace {
    // worst sections first; needs ConsistencyPlayLayer, so it lives below it
    std::vector<std::pair<double, size_t>> rankSections(PlayLayer* pl, int minAttempts) {
        std::vector<std::pair<double, size_t>> ranked;
        auto const& sections = static_cast<ConsistencyPlayLayer*>(pl)->m_fields->m_sections;

        for (size_t i = 0; i < sections.size(); i++) {
            double risk = riskOf(sections[i], minAttempts);
            if (risk > 0.0) ranked.emplace_back(risk, i);
        }
        std::sort(ranked.begin(), ranked.end(), [](auto const& a, auto const& b) { return a.first > b.first; });
        return ranked;
    }

    // report whichever kind of inconsistency is actually driving the score
    std::string describeSection(Section const& s, float percent) {
        double spread = std::clamp(s.spreadTicks() / SPREAD_CEILING_TICKS, 0.0, 1.0);
        double flip = 1.0 - std::abs(2.0 * s.clickRate() - 1.0);

        std::string detail = (s.n >= 2 && spread >= flip)
            ? fmt::format("spread {:.1f} ticks", s.spreadTicks())
            : fmt::format("pressed in {:.0f}% of runs", s.clickRate() * 100.0);

        return fmt::format("{:.0f}%  {}  ({} runs)", percent, detail, s.reached);
    }
}

class $modify(ConsistencyPauseLayer, PauseLayer) {
    void customSetup() {
        PauseLayer::customSetup();

        auto pl = PlayLayer::get();
        if (!pl || pl->m_levelLength <= 0.f) return;

        int minAttempts = static_cast<int>(Mod::get()->getSettingValue<int64_t>("min-attempts"));
        auto ranked = rankSections(pl, minAttempts);

        // one line, so the pause screen stays readable; the full list is a click away
        std::string summary;
        if (ranked.empty()) {
            summary = "Consistency Map: not enough attempts yet";
        }
        else {
            auto const& s = static_cast<ConsistencyPlayLayer*>(pl)->m_fields->m_sections[ranked[0].second];
            float percent = (static_cast<float>(ranked[0].second) * SECTION_WIDTH) / pl->m_levelLength * 100.f;
            summary = "Shakiest: " + describeSection(s, percent);
        }

        auto line = CCLabelBMFont::create(summary.c_str(), "chatFont.fnt");
        line->setScale(0.5f);
        line->setAnchorPoint(ccp(0.f, 0.5f));
        line->setPosition(ccp(16.f, 42.f));
        if (!ranked.empty()) line->setColor(colorForRisk(ranked[0].first));
        else line->setOpacity(160);
        this->addChild(line, 100);

        auto menu = CCMenu::create();
        menu->setPosition(CCPointZero);
        this->addChild(menu, 100);

        auto viewSprite = ButtonSprite::create("Map");
        viewSprite->setScale(0.45f);
        auto viewButton = CCMenuItemSpriteExtra::create(
            viewSprite, this, menu_selector(ConsistencyPauseLayer::onViewMap)
        );
        viewButton->setPosition(ccp(40.f, 18.f));
        menu->addChild(viewButton);

        auto gearSprite = CCSprite::createWithSpriteFrameName("GJ_optionsBtn_001.png");
        gearSprite->setScale(0.45f);
        auto gearButton = CCMenuItemSpriteExtra::create(
            gearSprite, this, menu_selector(ConsistencyPauseLayer::onSettings)
        );
        gearButton->setPosition(ccp(88.f, 18.f));
        menu->addChild(gearButton);
    }

    void onViewMap(CCObject*) {
        auto pl = PlayLayer::get();
        if (!pl) return;

        int minAttempts = static_cast<int>(Mod::get()->getSettingValue<int64_t>("min-attempts"));
        int wanted = static_cast<int>(Mod::get()->getSettingValue<int64_t>("panel-entries"));
        auto ranked = rankSections(pl, minAttempts);
        auto const& sections = static_cast<ConsistencyPlayLayer*>(pl)->m_fields->m_sections;

        std::string body;
        if (ranked.empty()) {
            body = fmt::format(
                "No section has been played through <cy>{}</c> times yet.\n\n"
                "Sections stay unscored until then, so the map reports your timing and not noise.",
                minAttempts
            );
        }
        else {
            for (int rank = 0; rank < std::min<int>(wanted, ranked.size()); rank++) {
                auto const& s = sections[ranked[rank].second];
                float percent = (static_cast<float>(ranked[rank].second) * SECTION_WIDTH) / pl->m_levelLength * 100.f;
                body += describeSection(s, percent) + "\n";
            }
        }

        FLAlertLayer::create("Consistency Map", body, "OK")->show();
    }

    void onSettings(CCObject*) {
        geode::openSettingsPopup(Mod::get());
    }
};
