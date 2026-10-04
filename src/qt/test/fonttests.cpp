// Copyright (c) 2026 The Smartiecoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/test/fonttests.h>

#include <qt/guiutil_font.h>

#include <QApplication>
#include <QFontDatabase>
#include <QFontMetrics>
#include <QTest>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <vector>

void FontTests::supportedWeights()
{
    // Exercise proportional, fixed-pitch and missing-family fallback fonts.
    // Unlike platform-specific expected weights, a native metrics oracle also
    // covers systems where multiple requested weights resolve to the same face.
    const std::array<QString, 3> families{
        QApplication::font().family(),
        QFontDatabase::systemFont(QFontDatabase::FixedFont).family(),
        QStringLiteral("Smartiecoin-test-missing-font-family"),
    };
    const std::array<QFont::Weight, 9> weights{
        QFont::Thin, QFont::ExtraLight, QFont::Light, QFont::Normal,
        QFont::Medium, QFont::DemiBold, QFont::Bold, QFont::ExtraBold, QFont::Black,
    };
    for (const auto& family : families) {
        std::vector<QFont::Weight> expected;
        auto width = [&](QFont::Weight weight) {
            QFont font;
            font.setFamily(family);
            font.setWeight(weight);
            font.setPointSizeF(GUIUtil::FontRegistry::DEFAULT_FONT_SIZE);
            return QFontMetrics(font).horizontalAdvance("Check the width of this text to see if the weight change has an impact!");
        };
        for (size_t i = 1; i < weights.size(); ++i) {
            if (width(weights[i - 1]) != width(weights[i])) {
                if (expected.empty()) expected.push_back(weights[i - 1]);
                expected.push_back(weights[i]);
            }
        }
        if (expected.empty()) expected.push_back(QFont::Normal);

        const GUIUtil::FontInfo actual{family};
        QVERIFY(actual.m_supported_weights == expected);
        auto closest = [&](QFont::Weight target) {
            return *std::min_element(expected.begin(), expected.end(), [target](auto a, auto b) {
                return std::abs(a - target) < std::abs(b - target);
            });
        };
        const auto normal = closest(GUIUtil::FontRegistry::TARGET_WEIGHT_NORMAL);
        auto bold = closest(GUIUtil::FontRegistry::TARGET_WEIGHT_BOLD);
        if (bold == normal) {
            auto next = std::find(expected.begin(), expected.end(), bold);
            if (++next != expected.end()) bold = *next;
        }
        QCOMPARE(actual.m_normal_default, normal);
        QCOMPARE(actual.m_bold_default, bold);
        QCOMPARE(actual.m_normal, normal);
        QCOMPARE(actual.m_bold, bold);

        GUIUtil::FontRegistry registry;
        const auto fonts_known = GUIUtil::g_fonts_known;
        const bool registered = registry.RegisterFont(family, false, true);
        GUIUtil::g_fonts_known = fonts_known;
        QVERIFY(registered);
        QVERIFY(registry.SetFont(family));
        QVERIFY(registry.GetSupportedWeights() == expected);
        for (size_t i = 0; i < expected.size(); ++i) {
            QCOMPARE(registry.WeightToIdx(expected[i]), int(i));
            QCOMPARE(registry.IdxToWeight(int(i)), expected[i]);
        }
    }
}
