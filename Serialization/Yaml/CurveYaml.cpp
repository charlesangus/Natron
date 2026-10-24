/* ***** BEGIN LICENSE BLOCK *****
 * This file is part of Natron <https://natrongithub.github.io/>,
 * (C) 2018-2023 The Natron developers
 * (C) 2013-2018 INRIA and Alexandre Gauthier-Foichat
 *
 * Natron is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * Natron is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Natron.  If not, see <http://www.gnu.org/licenses/gpl-2.0.html>
 * ***** END LICENSE BLOCK ***** */

#include "Serialization/Yaml/CurveYaml.h"

#include <cmath>
#include <optional>
#include <string>

#include "Serialization/Yaml/YamlPrivate.h"

NATRON_NAMESPACE_ENTER
namespace Serialization {

namespace YamlDetail {

    namespace {

        // The suffix that marks a free or broken keyframe whose tangents carry weights.
        constexpr char kWeightedTokenSuffix = 'W';

        bool
        keyframeHasWeights(const KeyFrameSerialization& key)
        {
            return keyframeStoresDerivatives(key.interpolation) && ((key.leftWeight != kKeyFrameDefaultTangentWeight) || (key.rightWeight != kKeyFrameDefaultTangentWeight));
        }

    } // namespace

    void
    emitCurve(YAML::Emitter& em,
              const CurveSerialization& curve)
    {
        em << YAML::Flow << YAML::BeginSeq;
        int previousInterpolation = -1;
        bool previousWeighted = false;
        for (const KeyFrameSerialization& key : curve.keyframes) {
            // Keys whose weights are both the default write no weights, so that a curve whose
            // handles were never lengthened is written exactly as before weights existed.
            const bool weighted = keyframeHasWeights(key);
            if ((key.interpolation != previousInterpolation) || (weighted != previousWeighted)) {
                std::string token(keyframeInterpolationToken(key.interpolation));
                if (weighted) {
                    token += kWeightedTokenSuffix;
                }
                em << token;
                previousInterpolation = key.interpolation;
                previousWeighted = weighted;
            }
            emitDouble(em, key.time);
            emitDouble(em, key.value);
            if (keyframeStoresDerivatives(key.interpolation)) {
                emitDouble(em, key.rightDerivative);
            }
            if (key.interpolation == kYamlKeyframeBroken) {
                emitDouble(em, key.leftDerivative);
            }
            if (weighted) {
                emitDouble(em, key.rightWeight);
                emitDouble(em, key.leftWeight);
            }
        }
        em << YAML::EndSeq;
    }

    namespace {

        enum CurveDecodeStateEnum {
            eCurveDecodeStateTimeOrInterpolation,
            eCurveDecodeStateTime,
            eCurveDecodeStateValue,
            eCurveDecodeStateRightDerivative,
            eCurveDecodeStateLeftDerivative,
            eCurveDecodeStateRightWeight,
            eCurveDecodeStateLeftWeight
        };

        const char*
        expectedAt(CurveDecodeStateEnum state)
        {
            switch (state) {
            case eCurveDecodeStateTimeOrInterpolation:
                return "a keyframe time or an interpolation token";
            case eCurveDecodeStateTime:
                return "a keyframe time";
            case eCurveDecodeStateValue:
                return "a keyframe value";
            case eCurveDecodeStateRightDerivative:
                return "a right derivative";
            case eCurveDecodeStateLeftDerivative:
                return "a left derivative";
            case eCurveDecodeStateRightWeight:
                return "a right weight";
            case eCurveDecodeStateLeftWeight:
                return "a left weight";
            }

            return "";
        }

        struct CurveToken {
            int interpolation;
            bool weighted;
        };

        std::optional<CurveToken>
        curveTokenFromText(const std::string& text)
        {
            if (std::optional<int> interpolation = keyframeInterpolationFromToken(text)) {
                return CurveToken { *interpolation, false };
            }
            if ((text.size() > 1) && (text.back() == kWeightedTokenSuffix)) {
                std::optional<int> interpolation = keyframeInterpolationFromToken(std::string_view(text).substr(0, text.size() - 1));
                if (interpolation && keyframeStoresDerivatives(*interpolation)) {
                    return CurveToken { *interpolation, true };
                }
            }

            return std::nullopt;
        }

    } // namespace

    CurveSerialization
    readCurve(const YAML::Node& node,
              const std::string& context)
    {
        CurveSerialization curve;
        if (!node || !node.IsSequence()) {
            fail(node, context, "expected a curve (a flow sequence of keyframes)");
        }
        curve.keyframes.reserve(node.size() / 2);

        CurveDecodeStateEnum state = eCurveDecodeStateTimeOrInterpolation;
        int interpolation = kYamlKeyframeLinear;
        bool weighted = false;
        KeyFrameSerialization key;
        auto pushKey = [&]() {
            if (!curve.keyframes.empty() && !(key.time > curve.keyframes.back().time)) {
                throw YamlError(context + ": keyframe " + std::to_string(curve.keyframes.size()) + " at time " + formatYamlDouble(key.time) + " does not come after the previous one" + where(node));
            }
            curve.keyframes.push_back(key);
            key = KeyFrameSerialization();
            state = eCurveDecodeStateTimeOrInterpolation;
        };

        for (std::size_t i = 0; i < node.size(); ++i) {
            const YAML::Node element = node[i];
            const std::string elementContext = context + ", curve element " + std::to_string(i);
            if (!isPlainScalar(element)) {
                fail(element, elementContext, std::string("expected ") + expectedAt(state));
            }
            const std::string& text = element.Scalar();
            std::optional<double> number = parseYamlDouble(text);
            if (!number) {
                std::optional<CurveToken> token = curveTokenFromText(text);
                if (!token) {
                    fail(element, elementContext, "\"" + text + "\" is neither a number nor an interpolation token");
                }
                if (state != eCurveDecodeStateTimeOrInterpolation) {
                    fail(element, elementContext, std::string("expected ") + expectedAt(state) + ", found the interpolation token \"" + text + "\"");
                }
                interpolation = token->interpolation;
                weighted = token->weighted;
                state = eCurveDecodeStateTime;
                continue;
            }

            switch (state) {
            case eCurveDecodeStateTimeOrInterpolation:
            case eCurveDecodeStateTime:
                if (std::isnan(*number)) {
                    fail(element, elementContext, "a keyframe time cannot be NaN");
                }
                key.time = *number;
                key.interpolation = interpolation;
                state = eCurveDecodeStateValue;
                break;
            case eCurveDecodeStateValue:
                key.value = *number;
                if (keyframeStoresDerivatives(key.interpolation)) {
                    state = eCurveDecodeStateRightDerivative;
                } else {
                    pushKey();
                }
                break;
            case eCurveDecodeStateRightDerivative:
                key.rightDerivative = *number;
                if (key.interpolation == kYamlKeyframeBroken) {
                    state = eCurveDecodeStateLeftDerivative;
                } else {
                    key.leftDerivative = *number;
                    if (weighted) {
                        state = eCurveDecodeStateRightWeight;
                    } else {
                        pushKey();
                    }
                }
                break;
            case eCurveDecodeStateLeftDerivative:
                key.leftDerivative = *number;
                if (weighted) {
                    state = eCurveDecodeStateRightWeight;
                } else {
                    pushKey();
                }
                break;
            case eCurveDecodeStateRightWeight:
            case eCurveDecodeStateLeftWeight:
                // NaN fails both comparisons. A weight outside the range would let time run
                // backwards within a segment, so it is refused rather than clamped.
                if (!((*number >= kKeyFrameMinTangentWeight) && (*number <= kKeyFrameMaxTangentWeight))) {
                    fail(element, elementContext, std::string(state == eCurveDecodeStateRightWeight ? "a right" : "a left") + " weight must be between " + formatYamlDouble(kKeyFrameMinTangentWeight) + " and " + formatYamlDouble(kKeyFrameMaxTangentWeight) + ", found " + text);
                }
                if (state == eCurveDecodeStateRightWeight) {
                    key.rightWeight = *number;
                    state = eCurveDecodeStateLeftWeight;
                } else {
                    key.leftWeight = *number;
                    pushKey();
                }
                break;
            }
        }
        if (state != eCurveDecodeStateTimeOrInterpolation) {
            fail(node, context, std::string("the curve ends where it expects ") + expectedAt(state));
        }

        return curve;
    }

} // namespace YamlDetail

std::string
encodeCurveYaml(const CurveSerialization& curve)
{
    YAML::Emitter em;
    YamlDetail::emitCurve(em, curve);

    return YamlDetail::documentText(em);
}

CurveSerialization
decodeCurveYaml(std::string_view text)
{
    YAML::Node node = YamlDetail::loadDocument(text, "the curve");

    return YamlDetail::readCurve(node, "curve");
}

CurveSerialization
canonicalCurveForYaml(const CurveSerialization& curve)
{
    CurveSerialization result = curve;
    for (KeyFrameSerialization& key : result.keyframes) {
        if (key.interpolation == kYamlKeyframeFree) {
            key.leftDerivative = key.rightDerivative;
        } else if (key.interpolation != kYamlKeyframeBroken) {
            key.leftDerivative = 0.;
            key.rightDerivative = 0.;
            key.leftWeight = kKeyFrameDefaultTangentWeight;
            key.rightWeight = kKeyFrameDefaultTangentWeight;
        }
    }

    return result;
}

} // namespace Serialization
NATRON_NAMESPACE_EXIT
