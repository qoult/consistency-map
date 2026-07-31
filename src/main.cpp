#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PauseLayer.hpp>
#include <Geode/ui/GeodeUI.hpp>
#include <Geode/ui/Popup.hpp>

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

    /*
    the level drawn as a bar. every section you have played is painted, so it
    fills in from the first attempt instead of staying blank until something is
    scored: grey for played but not judged yet, green for calm, yellow to red for
    shaky. calm and grey stretches are merged into single nodes, otherwise a long
    level would cost a thousand of them. shared by the in-game strip and the popup.
    */
    CCNode* buildStrip(
        std::vector<Section> const& sections, float levelLength,
        float barWidth, float height, int minAttempts, double threshold
    ) {
        auto strip = CCNode::create();

        // visible on its own, so an empty map still shows the mod is alive
        strip->addChild(CCLayerColor::create(ccc4(0, 0, 0, 160), barWidth, height));
        if (levelLength <= 0.f) return strip;

        float unit = barWidth / levelLength;
        size_t count = sections.size();

        auto paint = [&](size_t from, size_t to, ccColor3B color, GLubyte alpha) {
            float x = static_cast<float>(from) * SECTION_WIDTH * unit;
            float w = std::max(2.f, static_cast<float>(to - from) * SECTION_WIDTH * unit);
            if (x >= barWidth) return;
            w = std::min(w, barWidth - x);

            auto band = CCLayerColor::create(ccc4(color.r, color.g, color.b, alpha), w, height);
            band->setPositionX(x);
            strip->addChild(band);
        };

        // -2 never played, -1 played but not judged yet, 0 calm, 1 shaky
        auto classOf = [&](size_t i) {
            auto const& s = sections[i];
            if (s.reached <= 0) return -2;
            double risk = riskOf(s, minAttempts);
            if (risk < 0.0) return -1;
            return risk >= threshold ? 1 : 0;
        };

        size_t i = 0;
        while (i < count) {
            int cls = classOf(i);
            if (cls == -2) {
                i++;
                continue;
            }
            if (cls == 1) {
                // shaky sections keep their own shade, so they are never merged
                paint(i, i + 1, colorForRisk(riskOf(sections[i], minAttempts)), 235);
                i++;
                continue;
            }

            size_t j = i;
            while (j < count && classOf(j) == cls) j++;
            if (cls == 0) paint(i, j, ccc3(60, 200, 110), 150);
            else paint(i, j, ccc3(120, 124, 140), 90);
            i = j;
        }

        return strip;
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
        // left unknown on purpose: where the attempt really starts is whatever
        // position the first running frame reports. reading it here would mean
        // trusting that GD has already moved the player onto the start pos, and
        // if it hasn't, every section before the start pos would be recorded as
        // played and calm
        m_fields->m_startX = -1.f;
        m_fields->m_maxX = -1.f;
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

    void commitAttempt(char const* why) {
        if (m_fields->m_committed) {
            log::debug("commit skipped from {}: already committed", why);
            return;
        }
        if (!tracking()) {
            log::debug("commit skipped from {}: not tracking (practice {}, test {})", why, m_isPracticeMode, m_isTestMode);
            return;
        }
        // an attempt that never ran a frame tells us nothing
        if (m_fields->m_startX < 0.f) {
            log::debug("commit skipped from {}: attempt never ran a frame", why);
            return;
        }
        m_fields->m_committed = true;

        size_t from = sectionOf(m_fields->m_startX);
        size_t to = sectionOf(m_fields->m_maxX);
        for (size_t i = from; i <= to; i++) {
            auto& s = sectionAt(i);
            s.reached += 1;
            if (i < m_fields->m_clickedHere.size() && m_fields->m_clickedHere[i]) s.withClick += 1;
        }

        log::debug(
            "commit from {}: startX {:.0f} maxX {:.0f} -> sections {}..{}",
            why, m_fields->m_startX, m_fields->m_maxX, from, to
        );

        Mod::get()->setSavedValue<std::string>(m_fields->m_key, encodeSections(m_fields->m_sections));
    }

    void postUpdate(float dt) {
        PlayLayer::postUpdate(dt);
        if (!m_player1 || !tracking()) return;

        float x = m_player1->getPositionX();
        // first running frame of the attempt: this is where it actually starts,
        // start pos or not
        if (m_fields->m_startX < 0.f) {
            m_fields->m_startX = x;
            m_fields->m_maxX = x;
        }
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
        // commit before the original runs: GD's death handling may reset the
        // attempt underneath us, and the travelled distance would be gone
        if (player != m_player2) commitAttempt("death");
        PlayLayer::destroyPlayer(player, object);
    }

    void levelComplete() {
        commitAttempt("complete");
        PlayLayer::levelComplete();
    }

    void resetLevel() {
        // commit first: the original puts the player back at the start, and the
        // attempt that just ended still needs its distance counted
        commitAttempt("reset");
        PlayLayer::resetLevel();
        beginAttempt();
        rebuildStrip();
    }

    void onQuit() {
        commitAttempt("quit");
        PlayLayer::onQuit();
    }

    /*
    the strip sits where the progress bar is. every section you have played is
    painted, so the bar fills in from the first attempt instead of staying blank
    until something is scored: grey for played but not judged yet, green for
    calm, yellow to red for shaky. calm and grey stretches are merged into single
    nodes, otherwise a long level would cost a thousand of them.
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
        float height = 6.f;

        int minAttempts = static_cast<int>(Mod::get()->getSettingValue<int64_t>("min-attempts"));
        double threshold = Mod::get()->getSettingValue<double>("risk-threshold");

        auto strip = buildStrip(
            m_fields->m_sections, m_levelLength, barWidth, height, minAttempts, threshold
        );
        strip->setZOrder(1000);
        strip->setPosition(ccp(left, y));

        this->addChild(strip);
        m_fields->m_strip = strip;

        int visited = 0;
        int scored = 0;
        int mostRuns = 0;
        for (auto const& s : m_fields->m_sections) {
            if (s.reached > 0) visited++;
            if (s.reached >= minAttempts) scored++;
            mostRuns = std::max(mostRuns, s.reached);
        }

        log::debug(
            "strip built: levelLength {:.0f}, sections {}, visited {}, scored {}, most runs {}, bands {}, at ({:.0f}, {:.0f})",
            m_levelLength, m_fields->m_sections.size(), visited, scored, mostRuns,
            strip->getChildrenCount() - 1, left, y
        );
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
            // 0.0 means "scored, and rock solid" - only a negative risk means
            // there isn't enough data yet, and those are the ones to drop
            if (risk >= 0.0) ranked.emplace_back(risk, i);
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

class ConsistencyPopup : public geode::Popup {
protected:
    bool init(PlayLayer* pl) {
        int minAttempts = static_cast<int>(Mod::get()->getSettingValue<int64_t>("min-attempts"));
        double threshold = Mod::get()->getSettingValue<double>("risk-threshold");
        int wanted = static_cast<int>(Mod::get()->getSettingValue<int64_t>("panel-entries"));

        auto ranked = rankSections(pl, minAttempts);
        auto const& sections = static_cast<ConsistencyPlayLayer*>(pl)->m_fields->m_sections;
        int rows = std::min<int>(wanted, ranked.size());

        constexpr float ROW_HEIGHT = 24.f;
        constexpr float WIDTH = 400.f;
        float height = 132.f + std::max(1, rows) * ROW_HEIGHT;

        if (!Popup::init(WIDTH, height)) return false;
        this->setTitle("Consistency Map");

        float inner = WIDTH - 56.f;
        float left = (WIDTH - inner) / 2.f;

        // the level itself, same colours as the in-game strip
        auto map = buildStrip(sections, pl->m_levelLength, inner, 10.f, minAttempts, threshold);
        map->setPosition(ccp(left, height - 62.f));
        m_mainLayer->addChild(map);

        auto start = CCLabelBMFont::create("0%", "chatFont.fnt");
        start->setScale(0.4f);
        start->setAnchorPoint(ccp(0.f, 0.5f));
        start->setPosition(ccp(left, height - 72.f));
        start->setOpacity(120);
        m_mainLayer->addChild(start);

        auto finish = CCLabelBMFont::create("100%", "chatFont.fnt");
        finish->setScale(0.4f);
        finish->setAnchorPoint(ccp(1.f, 0.5f));
        finish->setPosition(ccp(left + inner, height - 72.f));
        finish->setOpacity(120);
        m_mainLayer->addChild(finish);

        float tableTop = height - 88.f;

        if (rows == 0) {
            int visited = 0;
            for (auto const& s : sections) if (s.reached > 0) visited++;

            auto empty = CCLabelBMFont::create(
                fmt::format("No section played {} times yet", minAttempts).c_str(), "bigFont.fnt"
            );
            empty->setScale(0.4f);
            empty->setPosition(ccp(WIDTH / 2.f, tableTop - 14.f));
            empty->setOpacity(180);
            m_mainLayer->addChild(empty);

            auto hint = CCLabelBMFont::create(
                fmt::format("{} sections visited so far - keep grinding", visited).c_str(), "chatFont.fnt"
            );
            hint->setScale(0.45f);
            hint->setPosition(ccp(WIDTH / 2.f, tableTop - 34.f));
            hint->setOpacity(120);
            m_mainLayer->addChild(hint);
        }
        else {
            // column headers
            auto addHeader = [&](char const* text, float x, float anchorX) {
                auto label = CCLabelBMFont::create(text, "chatFont.fnt");
                label->setScale(0.36f);
                label->setAnchorPoint(ccp(anchorX, 0.5f));
                label->setPosition(ccp(x, tableTop + 2.f));
                label->setOpacity(110);
                m_mainLayer->addChild(label);
            };
            addHeader("AT", left + 16.f, 0.f);
            addHeader("WHAT MAKES IT SHAKY", left + 62.f, 0.f);
            addHeader("RUNS", left + inner - 4.f, 1.f);

            for (int rank = 0; rank < rows; rank++) {
                auto const& s = sections[ranked[rank].second];
                double risk = ranked[rank].first;
                float y = tableTop - 12.f - rank * ROW_HEIGHT;
                float percent = (static_cast<float>(ranked[rank].second) * SECTION_WIDTH) / pl->m_levelLength * 100.f;

                // alternating bands, so long rows stay readable
                auto band = CCLayerColor::create(
                    ccc4(255, 255, 255, rank % 2 == 0 ? 18 : 8), inner, ROW_HEIGHT - 2.f
                );
                band->setPosition(ccp(left, y - (ROW_HEIGHT - 2.f) / 2.f));
                m_mainLayer->addChild(band);

                auto color = colorForRisk(risk);
                auto chip = CCLayerColor::create(ccc4(color.r, color.g, color.b, 255), 4.f, ROW_HEIGHT - 8.f);
                chip->setPosition(ccp(left + 6.f, y - (ROW_HEIGHT - 8.f) / 2.f));
                m_mainLayer->addChild(chip);

                auto at = CCLabelBMFont::create(fmt::format("{:.0f}%", percent).c_str(), "bigFont.fnt");
                at->setScale(0.34f);
                at->setAnchorPoint(ccp(0.f, 0.5f));
                at->setPosition(ccp(left + 16.f, y));
                at->setColor(color);
                m_mainLayer->addChild(at);

                double spread = std::clamp(s.spreadTicks() / SPREAD_CEILING_TICKS, 0.0, 1.0);
                double flip = 1.0 - std::abs(2.0 * s.clickRate() - 1.0);
                std::string detail = (s.n >= 2 && spread >= flip)
                    ? fmt::format("clicks scatter over {:.1f} ticks", s.spreadTicks())
                    : (s.clickRate() > 0.99
                        ? "always pressed, steady timing"
                        : fmt::format("pressed in only {:.0f}% of runs", s.clickRate() * 100.0));

                auto what = CCLabelBMFont::create(detail.c_str(), "chatFont.fnt");
                what->setScale(0.5f);
                what->setAnchorPoint(ccp(0.f, 0.5f));
                what->setPosition(ccp(left + 62.f, y));
                m_mainLayer->addChild(what);

                auto runs = CCLabelBMFont::create(std::to_string(s.reached).c_str(), "chatFont.fnt");
                runs->setScale(0.5f);
                runs->setAnchorPoint(ccp(1.f, 0.5f));
                runs->setPosition(ccp(left + inner - 4.f, y));
                runs->setOpacity(150);
                m_mainLayer->addChild(runs);
            }
        }

        int visited = 0;
        int scored = 0;
        for (auto const& s : sections) {
            if (s.reached > 0) visited++;
            if (s.reached >= minAttempts) scored++;
        }

        auto footer = CCLabelBMFont::create(
            fmt::format("{} scored - {} visited - a section needs {} runs", scored, visited, minAttempts).c_str(),
            "chatFont.fnt"
        );
        footer->setScale(0.4f);
        footer->setPosition(ccp(WIDTH / 2.f, 22.f));
        footer->setOpacity(110);
        m_mainLayer->addChild(footer);

        return true;
    }

public:
    static ConsistencyPopup* create(PlayLayer* pl) {
        auto ret = new ConsistencyPopup();
        if (ret->init(pl)) {
            ret->autorelease();
            return ret;
        }
        CC_SAFE_DELETE(ret);
        return nullptr;
    }
};

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
            summary = fmt::format("Consistency Map: no section played {} times yet", minAttempts);
        }
        else if (ranked[0].first < 0.15) {
            // scored and calm is a real answer, not the same as having no data
            summary = fmt::format("Consistency Map: all clear ({} sections scored)", ranked.size());
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
            gearSprite, this, menu_selector(ConsistencyPauseLayer::onModSettings)
        );
        gearButton->setPosition(ccp(88.f, 18.f));
        menu->addChild(gearButton);
    }

    void onViewMap(CCObject*) {
        auto pl = PlayLayer::get();
        if (!pl) return;

        if (auto popup = ConsistencyPopup::create(pl)) popup->show();
    }

    // NOT onSettings: PauseLayer already has one, and a matching name in a
    // $modify class hooks the game's function instead of adding a new method
    void onModSettings(CCObject*) {
        geode::openSettingsPopup(Mod::get());
    }
};
